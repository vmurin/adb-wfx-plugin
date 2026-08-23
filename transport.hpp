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
#include <sys/types.h>
#include <unistd.h>

namespace transport_detail {

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
// done.
inline bool loopTransfer(const RawTransferFn& rawTransfer, size_t n) {
    size_t done = 0;
    while (done < n) {
        ptrdiff_t result = rawTransfer(done, n - done);
        if (result < 0) {
            if (errno == EINTR) {
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

    // Reads up to n bytes in a single underlying call. Returns the number
    // of bytes read, 0 on EOF, -1 on error (including EINTR -- readSome
    // itself does not retry; that guarantee belongs to writeAll and
    // readExactly, via their shared retry loop,
    // transport_detail::loopTransfer).
    virtual ptrdiff_t readSome(void* buf, size_t n) = 0;

    virtual void close() = 0;

    virtual std::string lastError() const = 0;
};

class TcpTransport : public Transport {
public:
    ~TcpTransport() override {
        close();
    }

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    // Connects to host:port over TCP. Returns nullptr on failure, with
    // *error set to a human-readable message.
    static std::unique_ptr<TcpTransport> connectTo(const std::string& host,
                                                    int port, std::string* error) {
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
                    if (errno != EINTR) {
                        setErrnoMessage(&lastError_, "write");
                    }
                    return -1;
                }
                if (result == 0) {
                    lastError_ = "write: connection closed";
                }
                return static_cast<ptrdiff_t>(result);
            },
            n);
        return ok;
    }

    bool readExactly(void* buf, size_t n) override {
        char* p = static_cast<char*>(buf);
        return transport_detail::loopTransfer(
            [this, p](size_t offset, size_t remaining) {
                return readSome(p + offset, remaining);
            },
            n);
    }

    ptrdiff_t readSome(void* buf, size_t n) override {
        ssize_t result = ::recv(fd_, buf, n, 0);
        if (result < 0) {
            if (errno != EINTR) {
                setErrnoMessage(&lastError_, "read");
            }
            return -1;
        }
        return static_cast<ptrdiff_t>(result);
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
// failure). AdbClient (Task 7) is built against this, never a bare socket,
// so it can be driven by FakeTransport in tests.
using TransportFactory = std::function<std::unique_ptr<Transport>(std::string* error)>;

#endif // ADB_WFX_TRANSPORT_HPP
