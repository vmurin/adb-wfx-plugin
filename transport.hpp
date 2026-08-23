// The I/O seam: the only place in this codebase permitted to touch a
// socket. `Transport` is a pure interface; `TcpTransport` is the sole
// concrete implementation that talks TCP. Everything downstream (AdbClient,
// PluginCore) is built against `Transport` / `TransportFactory` so it can be
// exercised in tests with no phone and no ADB server attached -- see
// tests/fake_transport.hpp for the test double used by Tasks 6-9.
#ifndef ADB_WFX_TRANSPORT_HPP
#define ADB_WFX_TRANSPORT_HPP

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

// How long a single blocking read or write on the adb socket may sit
// without moving a byte before the kernel hands control back with
// EAGAIN/EWOULDBLOCK (SO_RCVTIMEO/SO_SNDTIMEO, set in
// TcpTransport::connectTo). This is NOT a deadline for the operation:
// hitting it polls the stall callback and, unless that says to stop,
// waits again. Its only job is to stop a sleeping phone or a
// renegotiating USB link from parking the caller inside recv()/send()
// where the Cancel button cannot reach it.
constexpr int SOCKET_TIMEOUT_MS = 30000;

// Installed on a Transport by a caller that must stay responsive while a
// transfer stalls (see Transport::setStallCallback). Returns true to keep
// waiting, false to abandon the pending read or write.
using StallFn = std::function<bool()>;

namespace transport_detail {

// True for the errno a socket with SO_RCVTIMEO/SO_SNDTIMEO reports when
// the timeout expires with nothing transferred. POSIX names EAGAIN;
// EWOULDBLOCK is the same value on Darwin but is spelled out separately
// because it is not required to be.
inline bool isTimeoutErrno(int errnoValue) {
    return errnoValue == EAGAIN || errnoValue == EWOULDBLOCK;
}

// "Should a stalled transfer wait some more?" -- true when no stall
// callback is installed (the pre-timeout behaviour: wait indefinitely),
// otherwise whatever the callback says.
inline bool stallShouldKeepWaiting(const StallFn& onStall) {
    return !onStall || onStall();
}

// One raw transfer attempt, with send()/recv() semantics: called with how
// many bytes are already done and how many remain, it returns the number
// of bytes transferred this attempt, 0 for EOF/closed, or -1 with errno
// set (including EINTR, which loopTransfer below retries). Takes an
// offset/remaining pair rather than a raw pointer so the loop can be
// driven directly by a test with no real buffer at all.
using RawTransferFn = std::function<ptrdiff_t(size_t offset, size_t remaining)>;

// The retry loop shared by TcpTransport::writeAll and
// TcpTransport::readExactly: drives `rawTransfer` until `n` bytes have
// been moved in total, looping over partial transfers and retrying a
// transfer that failed with EINTR. Stops and returns false on any other
// error, or on a 0-byte result (EOF / peer closed) before n bytes are
// done. (readExactly's `rawTransfer` calls readSome, which already retries
// EINTR itself via retryOnEintr below -- so on that path this EINTR branch
// is never actually taken; it stays live and tested here because
// writeAll's `rawTransfer`, a raw send(), does not retry on its own.)
inline bool loopTransfer(const RawTransferFn& rawTransfer, size_t n,
                         const StallFn& onStall = nullptr) {
    size_t done = 0;
    while (done < n) {
        ptrdiff_t result = rawTransfer(done, n - done);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            // A socket timeout is not a failure -- nothing moved, that is
            // all. Poll the stall callback (the caller's Cancel button)
            // and, unless it gives up, keep waiting.
            if (isTimeoutErrno(errno) && stallShouldKeepWaiting(onStall)) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        done += static_cast<size_t>(result);
    }
    return true;
}

// One raw single-shot attempt, with recv()/send() semantics: returns
// bytes transferred, 0 for EOF, or -1 with errno set (including EINTR).
using RawAttemptFn = std::function<ptrdiff_t()>;

// Retries `rawAttempt` while it fails with EINTR, returning its first
// non-EINTR result (success, EOF, or a real error) unchanged. This is the
// seam TcpTransport::readSome uses to hide a transient signal interruption
// from every caller -- not only from readExactly's loop above, since
// readSome is also called directly by streaming code (e.g. AdbClient
// reading sync DATA chunks or draining shell: output).
inline ptrdiff_t retryOnEintr(const RawAttemptFn& rawAttempt,
                              const StallFn& onStall = nullptr) {
    for (;;) {
        ptrdiff_t result = rawAttempt();
        if (result < 0 && errno == EINTR) {
            continue;
        }
        // Same reasoning as loopTransfer's timeout branch above. Note the
        // result handed back on a cancelled stall is the raw -1, never 0:
        // reporting a stalled socket as EOF would silently truncate a
        // download into a "successful" short file.
        if (result < 0 && isTimeoutErrno(errno) && stallShouldKeepWaiting(onStall)) {
            continue;
        }
        return result;
    }
}

} // namespace transport_detail

class Transport {
public:
    virtual ~Transport() = default;

    // Writes exactly n bytes, looping over partial writes and retrying on
    // EINTR. Returns false on error (including a broken pipe -- SIGPIPE is
    // guarded against separately, see TcpTransport::connectTo).
    virtual bool writeAll(const void* buf, size_t n) = 0;

    // Reads exactly n bytes, looping over short reads and retrying on
    // EINTR. Returns false on EOF or error before n bytes were read.
    virtual bool readExactly(void* buf, size_t n) = 0;

    // Reads up to n bytes in a single underlying call, transparently
    // retrying on EINTR. Returns the number of bytes read, 0 on EOF, -1 on
    // a real error.
    virtual ptrdiff_t readSome(void* buf, size_t n) = 0;

    virtual void close() = 0;

    virtual std::string lastError() const = 0;

    // Installs (or, with an empty StallFn, removes) the hook consulted
    // whenever a read or write on this transport sits SOCKET_TIMEOUT_MS
    // without moving a byte. Returning true keeps waiting; returning
    // false abandons the pending operation, which then fails rather than
    // reporting EOF. AdbClient wires this to the caller's ProgressFn
    // during syncRecv/syncSend so Cancel works while the phone is
    // asleep, instead of only at the next chunk boundary. Non-virtual
    // and shared by every Transport, the test double included; with no
    // hook installed a stall simply waits, exactly as it did before
    // timeouts existed.
    void setStallCallback(StallFn onStall) {
        onStall_ = std::move(onStall);
    }

protected:
    const StallFn& stallCallback() const {
        return onStall_;
    }

private:
    StallFn onStall_;
};

class TcpTransport : public Transport {
public:
    ~TcpTransport() override {
        close();
    }

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    // Connects to host:port over TCP. Returns nullptr on failure, with
    // *error set to a human-readable message. timeoutMs is the per-call
    // read/write timeout installed on the socket (see SOCKET_TIMEOUT_MS);
    // it is a parameter only so tests can prove the stall path in
    // milliseconds instead of waiting out the real one.
    static std::unique_ptr<TcpTransport> connectTo(const std::string& host,
                                                    int port, std::string* error,
                                                    int timeoutMs = SOCKET_TIMEOUT_MS) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            setErrnoMessage(error, "socket");
            return nullptr;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            if (error != nullptr) {
                *error = "invalid host address: " + host;
            }
            ::close(fd);
            return nullptr;
        }

        int connectResult;
        do {
            connectResult = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        } while (connectResult < 0 && errno == EINTR);

        if (connectResult < 0) {
            setErrnoMessage(error, "connect");
            ::close(fd);
            return nullptr;
        }

        // Without these, recv()/send() block forever: a phone that goes
        // to sleep mid-pull parks the calling thread inside the kernel,
        // where the cancellation check between chunks never runs and the
        // Cancel button does nothing. With them, a stall surfaces every
        // timeoutMs as EAGAIN, which loopTransfer/retryOnEintr turn into
        // a poll of the stall callback rather than an error.
        struct timeval timeout;
        timeout.tv_sec = timeoutMs / MILLISECONDS_PER_SECOND;
        timeout.tv_usec = (timeoutMs % MILLISECONDS_PER_SECOND) * MICROSECONDS_PER_MILLISECOND;
        if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
            setErrnoMessage(error, "setsockopt(SO_RCVTIMEO/SO_SNDTIMEO)");
            ::close(fd);
            return nullptr;
        }

#ifdef SO_NOSIGPIPE
        // Darwin has no MSG_NOSIGNAL; this is the per-socket equivalent.
        // A broken pipe must never raise SIGPIPE and kill the host process
        // (this code runs inside Double Commander, not a standalone
        // binary), so failing to install the guard fails the connection
        // rather than silently running unprotected.
        int on = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) != 0) {
            setErrnoMessage(error, "setsockopt(SO_NOSIGPIPE)");
            ::close(fd);
            return nullptr;
        }
#endif

        return std::unique_ptr<TcpTransport>(new TcpTransport(fd));
    }

    bool writeAll(const void* buf, size_t n) override {
        const char* p = static_cast<const char*>(buf);
        bool ok = transport_detail::loopTransfer(
            [this, p](size_t offset, size_t remaining) -> ptrdiff_t {
                ssize_t result = sendNoSignal(p + offset, remaining);
                if (result < 0) {
                    // EINTR is retried by the loop and a timeout may yet
                    // be waited through; neither is the error the caller
                    // should end up seeing, so neither overwrites
                    // lastError_. The timeout text is set anyway because
                    // it IS what the caller sees if the stall callback
                    // then gives up.
                    if (transport_detail::isTimeoutErrno(errno)) {
                        lastError_ = STALL_CANCELLED_MESSAGE;
                    } else if (errno != EINTR) {
                        setErrnoMessage(&lastError_, "write");
                    }
                    return -1;
                }
                if (result == 0) {
                    lastError_ = "write: connection closed";
                }
                return static_cast<ptrdiff_t>(result);
            },
            n, stallCallback());
        return ok;
    }

    bool readExactly(void* buf, size_t n) override {
        char* p = static_cast<char*>(buf);
        return transport_detail::loopTransfer(
            [this, p](size_t offset, size_t remaining) {
                return readSome(p + offset, remaining);
            },
            // The stall callback MUST be passed here, exactly as writeAll
            // does. readSome consults it on its own, but when it answers
            // -1/EAGAIN *because the callback said stop*, this loop sees
            // a timeout errno -- and without the callback it would ask
            // stallShouldKeepWaiting(nullptr), get "keep waiting", and go
            // straight back into readSome to stall and cancel again,
            // forever. syncRecv reads every sync header and every DATA
            // chunk through here, so that is Cancel hanging a download.
            n, stallCallback());
    }

    ptrdiff_t readSome(void* buf, size_t n) override {
        ptrdiff_t result = transport_detail::retryOnEintr(
            [this, buf, n]() -> ptrdiff_t {
                return static_cast<ptrdiff_t>(::recv(fd_, buf, n, 0));
            },
            stallCallback());
        if (result < 0) {
            // Reaching here with a timeout errno means the stall callback
            // gave up waiting -- report that, not "Resource temporarily
            // unavailable", which describes the kernel's view and not the
            // user's.
            if (transport_detail::isTimeoutErrno(errno)) {
                lastError_ = STALL_CANCELLED_MESSAGE;
            } else {
                setErrnoMessage(&lastError_, "read");
            }
        }
        return result;
    }

    void close() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    std::string lastError() const override {
        return lastError_;
    }

private:
    explicit TcpTransport(int fd) : fd_(fd) {}

    static constexpr int MILLISECONDS_PER_SECOND = 1000;
    static constexpr int MICROSECONDS_PER_MILLISECOND = 1000;

    // What lastError() reports when a stall callback abandoned a wait.
    // Deliberately says "cancelled": by construction the only way to get
    // here is a caller's own stall callback returning false.
    static constexpr const char* STALL_CANCELLED_MESSAGE =
        "transfer cancelled while waiting for the device";

    // send() with SIGPIPE suppressed. MSG_NOSIGNAL does not exist on
    // Darwin, so the guard is SO_NOSIGPIPE (set once, in connectTo); this
    // wrapper exists so the flag can still be passed on platforms that do
    // have MSG_NOSIGNAL.
    ssize_t sendNoSignal(const void* buf, size_t n) {
#ifdef MSG_NOSIGNAL
        return ::send(fd_, buf, n, MSG_NOSIGNAL);
#else
        return ::send(fd_, buf, n, 0);
#endif
    }

    static void setErrnoMessage(std::string* error, const char* what) {
        if (error != nullptr) {
            *error = std::string(what) + ": " + std::strerror(errno);
        }
    }

    int fd_ = -1;
    std::string lastError_;
};

// A factory that produces a fresh Transport (or nullptr + *error on
// failure). AdbClient is built against this, never a bare socket,
// so it can be driven by FakeTransport in tests.
using TransportFactory = std::function<std::unique_ptr<Transport>(std::string* error)>;

#endif // ADB_WFX_TRANSPORT_HPP
