// Tests for transport.hpp (the I/O seam) and tests/fake_transport.hpp (the
// scripted double Tasks 6-9 are built against). Written before either
// header existed (TDD) -- see
// .superpowers/sdd/plan-adb-wfx/task-5-report.md for the RED run.
#include "transport.hpp"
#include "fake_transport.hpp"
#include "testing.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <future>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// Binds a TCP listener to an OS-assigned loopback port and returns the fd
// plus the assigned port (read back via getsockname -- never a hardcoded
// port, so tests never collide with something else on the machine).
int startListener(int* outPort) {
    int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(listenFd, 1);

    sockaddr_in bound{};
    socklen_t boundLen = sizeof(bound);
    ::getsockname(listenFd, reinterpret_cast<sockaddr*>(&bound), &boundLen);
    *outPort = ntohs(bound.sin_port);
    return listenFd;
}

} // namespace

// ---------------------------------------------------------------------
// FakeTransport
// ---------------------------------------------------------------------

TEST(FakeTransportSuite, writesAreRecordedVerbatim) {
    FakeTransport transport{std::string()};
    CHECK(transport.writeAll("abc", 3));
    CHECK(transport.writeAll("de", 2));
    CHECK_STR_EQ(transport.written(), "abcde");
}

TEST(FakeTransportSuite, scriptedReadsComeBackInOrder) {
    std::vector<std::string> chunks = {"hello", "world"};
    FakeTransport transport(chunks);

    char buf1[5];
    CHECK(transport.readExactly(buf1, 5));
    CHECK(std::memcmp(buf1, "hello", 5) == 0);

    char buf2[5];
    CHECK(transport.readExactly(buf2, 5));
    CHECK(std::memcmp(buf2, "world", 5) == 0);
}

TEST(FakeTransportSuite, readExactlyAcrossChunkBoundaryWorks) {
    std::vector<std::string> chunks = {"ab", "cd", "ef"};
    FakeTransport transport(chunks);

    char buf[6];
    CHECK(transport.readExactly(buf, 6));
    CHECK(std::memcmp(buf, "abcdef", 6) == 0);
}

TEST(FakeTransportSuite, readExactlyPastEofReturnsFalse) {
    FakeTransport transport(std::string("ab"));
    char buf[4];
    CHECK(!transport.readExactly(buf, 4));
}

TEST(FakeTransportSuite, readSomeReturnsPartialChunks) {
    FakeTransport transport(std::string("abcdef"));
    char buf[3];
    ptrdiff_t n = transport.readSome(buf, 3);
    CHECK_EQ(n, 3);
    CHECK(std::memcmp(buf, "abc", 3) == 0);

    ptrdiff_t n2 = transport.readSome(buf, 3);
    CHECK_EQ(n2, 3);
    CHECK(std::memcmp(buf, "def", 3) == 0);
}

TEST(FakeTransportSuite, failAfterBytesFailsSubsequentReads) {
    FakeTransport transport(std::string("abcdef"));
    transport.failAfterBytes(3);

    char buf[3];
    CHECK(transport.readExactly(buf, 3));
    CHECK(std::memcmp(buf, "abc", 3) == 0);

    char buf2[3];
    CHECK(!transport.readExactly(buf2, 3));
}

TEST(FakeTransportSuite, failWritesAfterBytesFailsSubsequentWrites) {
    FakeTransport transport{std::string()};
    transport.failWritesAfterBytes(3);

    CHECK(transport.writeAll("abc", 3));
    CHECK(!transport.writeAll("d", 1));
}

// ---------------------------------------------------------------------
// transport_detail::loopTransfer -- the retry loop shared by writeAll and
// readExactly, driven directly against a fake raw transfer function so the
// multi-iteration and EINTR-retry paths are provably exercised (a
// blocking socket send() can complete a 200000-byte write in one call, so
// the real-socket tests below cannot prove the loop actually loops).
// ---------------------------------------------------------------------

TEST(LoopTransferSuite, accumulatesAcrossMultipleShortTransfers) {
    std::vector<size_t> offsetsSeen;
    auto raw = [&](size_t offset, size_t remaining) -> ptrdiff_t {
        offsetsSeen.push_back(offset);
        size_t chunk = std::min<size_t>(remaining, 3);
        return static_cast<ptrdiff_t>(chunk);
    };

    bool ok = transport_detail::loopTransfer(raw, 10);

    CHECK(ok);
    std::vector<size_t> expected = {0, 3, 6, 9};
    CHECK(offsetsSeen == expected);
}

TEST(LoopTransferSuite, eintrRetriesWithoutLosingOrDroppingProgress) {
    std::vector<size_t> offsetsSeen;
    int callCount = 0;
    auto raw = [&](size_t offset, size_t remaining) -> ptrdiff_t {
        offsetsSeen.push_back(offset);
        ++callCount;
        if (callCount == 2) {
            errno = EINTR;
            return -1;
        }
        size_t chunk = std::min<size_t>(remaining, 4);
        return static_cast<ptrdiff_t>(chunk);
    };

    bool ok = transport_detail::loopTransfer(raw, 10);

    CHECK(ok);
    // call 1: offset 0, transfers 4 (done=4).
    // call 2: offset 4, EINTR -- no progress, retried at the same offset.
    // call 3: offset 4 again, transfers 4 (done=8).
    // call 4: offset 8, transfers the remaining 2 (done=10).
    std::vector<size_t> expected = {0, 4, 4, 8};
    CHECK(offsetsSeen == expected);
}

TEST(LoopTransferSuite, nonEintrErrorStopsTheLoop) {
    int callCount = 0;
    auto raw = [&](size_t, size_t) -> ptrdiff_t {
        ++callCount;
        if (callCount == 1) {
            return 3;
        }
        errno = EIO;
        return -1;
    };

    bool ok = transport_detail::loopTransfer(raw, 10);

    CHECK(!ok);
    CHECK_EQ(callCount, 2);
}

TEST(LoopTransferSuite, zeroResultStopsTheLoop) {
    auto raw = [](size_t, size_t) -> ptrdiff_t {
        return 0;
    };

    CHECK(!transport_detail::loopTransfer(raw, 5));
}

// ---------------------------------------------------------------------
// The stall hook. connectTo now sets SO_RCVTIMEO/SO_SNDTIMEO, so a socket
// that stops moving (the phone sleeps mid-pull, USB renegotiates) reports
// EAGAIN/EWOULDBLOCK instead of blocking forever. That is not an error
// and must never be reported as one: the loops below poll the stall
// callback -- which is how AdbClient re-checks the user's Cancel button
// while stuck inside a single recv()/send() -- and keep waiting unless it
// says to stop. With no callback installed the behaviour is the old one:
// wait indefinitely.
// ---------------------------------------------------------------------

TEST(LoopTransferSuite, timeoutKeepsWaitingWhenNoStallCallbackIsInstalled) {
    int callCount = 0;
    auto raw = [&](size_t, size_t remaining) -> ptrdiff_t {
        ++callCount;
        if (callCount <= 2) {
            errno = EAGAIN;
            return -1;
        }
        return static_cast<ptrdiff_t>(remaining);
    };

    CHECK(transport_detail::loopTransfer(raw, 5));
    CHECK_EQ(callCount, 3); // two timeouts waited through, then the real transfer
}

TEST(LoopTransferSuite, timeoutPollsTheStallCallbackAndKeepsWaitingWhenItSaysSo) {
    int stallPolls = 0;
    auto onStall = [&]() -> bool {
        ++stallPolls;
        return true; // "the user has not cancelled"
    };
    int callCount = 0;
    auto raw = [&](size_t, size_t remaining) -> ptrdiff_t {
        ++callCount;
        if (callCount == 1) {
            errno = EWOULDBLOCK;
            return -1;
        }
        return static_cast<ptrdiff_t>(remaining);
    };

    CHECK(transport_detail::loopTransfer(raw, 4, onStall));
    CHECK_EQ(stallPolls, 1);
}

TEST(LoopTransferSuite, timeoutStopsTheLoopWhenTheStallCallbackCancels) {
    auto onStall = []() -> bool {
        return false; // "Cancel was pressed"
    };
    int callCount = 0;
    auto raw = [&](size_t, size_t) -> ptrdiff_t {
        ++callCount;
        errno = EAGAIN;
        return -1;
    };

    CHECK(!transport_detail::loopTransfer(raw, 4, onStall));
    CHECK_EQ(callCount, 1); // abandoned at the first stall, not retried
}

TEST(RetryOnEintrSuite, timeoutKeepsWaitingWhenNoStallCallbackIsInstalled) {
    int callCount = 0;
    auto raw = [&]() -> ptrdiff_t {
        ++callCount;
        if (callCount <= 2) {
            errno = EAGAIN;
            return -1;
        }
        return 9;
    };

    CHECK_EQ(transport_detail::retryOnEintr(raw), 9);
    CHECK_EQ(callCount, 3);
}

TEST(RetryOnEintrSuite, timeoutCancelledByTheStallCallbackReportsAnErrorNotEof) {
    int stallPolls = 0;
    auto onStall = [&]() -> bool {
        ++stallPolls;
        return false;
    };
    auto raw = [&]() -> ptrdiff_t {
        errno = EAGAIN;
        return -1;
    };

    ptrdiff_t result = transport_detail::retryOnEintr(raw, onStall);

    // -1, never 0: a stalled socket that the user gave up on is an error,
    // and misreporting it as EOF would truncate a download silently.
    CHECK_EQ(result, -1);
    CHECK_EQ(stallPolls, 1);
}

TEST(IsTimeoutErrnoSuite, recognisesTheSocketTimeoutErrnosAndNothingElse) {
    CHECK(transport_detail::isTimeoutErrno(EAGAIN));
    CHECK(transport_detail::isTimeoutErrno(EWOULDBLOCK));
    CHECK(!transport_detail::isTimeoutErrno(EINTR));
    CHECK(!transport_detail::isTimeoutErrno(EIO));
    CHECK(!transport_detail::isTimeoutErrno(EPIPE));
}

// ---------------------------------------------------------------------
// transport_detail::retryOnEintr -- the seam TcpTransport::readSome uses to
// hide a transient EINTR from every caller, not just readExactly (Task 7's
// AdbClient calls readSome directly to stream sync DATA chunks and drain
// shell: output, so a bare EINTR must never surface to it as a hard -1).
// ---------------------------------------------------------------------

TEST(RetryOnEintrSuite, retriesOnEintrThenReturnsTheRealResult) {
    int callCount = 0;
    auto raw = [&]() -> ptrdiff_t {
        ++callCount;
        if (callCount == 1) {
            errno = EINTR;
            return -1;
        }
        return 7;
    };

    ptrdiff_t result = transport_detail::retryOnEintr(raw);

    CHECK_EQ(result, 7);
    CHECK_EQ(callCount, 2);
}

TEST(RetryOnEintrSuite, passesThroughANonEintrErrorWithoutRetrying) {
    int callCount = 0;
    auto raw = [&]() -> ptrdiff_t {
        ++callCount;
        errno = EIO;
        return -1;
    };

    ptrdiff_t result = transport_detail::retryOnEintr(raw);

    CHECK_EQ(result, -1);
    CHECK_EQ(callCount, 1);
}

TEST(RetryOnEintrSuite, passesThroughEofWithoutRetrying) {
    int callCount = 0;
    auto raw = [&]() -> ptrdiff_t {
        ++callCount;
        return 0;
    };

    ptrdiff_t result = transport_detail::retryOnEintr(raw);

    CHECK_EQ(result, 0);
    CHECK_EQ(callCount, 1);
}

// ---------------------------------------------------------------------
// TcpTransport
// ---------------------------------------------------------------------

TEST(TcpTransportSuite, echoRoundTripThroughRealSocket) {
    int port = 0;
    int listenFd = startListener(&port);

    std::thread server([listenFd]() {
        int connFd = ::accept(listenFd, nullptr, nullptr);
        char buf[3];
        size_t got = 0;
        while (got < sizeof(buf)) {
            ssize_t r = ::recv(connFd, buf + got, sizeof(buf) - got, 0);
            if (r <= 0) {
                break;
            }
            got += static_cast<size_t>(r);
        }
        size_t sent = 0;
        while (sent < got) {
            ssize_t w = ::send(connFd, buf + sent, got - sent, 0);
            if (w <= 0) {
                break;
            }
            sent += static_cast<size_t>(w);
        }
        ::close(connFd);
    });

    std::string error;
    std::unique_ptr<TcpTransport> transport = TcpTransport::connectTo("127.0.0.1", port, &error);
    CHECK(transport != nullptr);

    CHECK(transport->writeAll("xyz", 3));
    char reply[3] = {};
    CHECK(transport->readExactly(reply, 3));
    CHECK(std::memcmp(reply, "xyz", 3) == 0);

    transport->close();
    server.join();
    ::close(listenFd);
}

TEST(TcpTransportSuite, writeAllDeliversEverythingToSlowReader) {
    int port = 0;
    int listenFd = startListener(&port);

    const size_t totalBytes = 200000;
    std::thread server([listenFd]() {
        int connFd = ::accept(listenFd, nullptr, nullptr);
        char chunk[4096];
        size_t got = 0;
        while (got < totalBytes) {
            size_t want = std::min(sizeof(chunk), totalBytes - got);
            ssize_t r = ::recv(connFd, chunk, want, 0);
            if (r <= 0) {
                break;
            }
            got += static_cast<size_t>(r);
        }
        ::close(connFd);
    });

    std::string error;
    std::unique_ptr<TcpTransport> transport = TcpTransport::connectTo("127.0.0.1", port, &error);
    CHECK(transport != nullptr);

    std::string payload(totalBytes, 'z');
    CHECK(transport->writeAll(payload.data(), payload.size()));

    transport->close();
    server.join();
    ::close(listenFd);
}

TEST(TcpTransportSuite, connectToClosedPortFailsWithError) {
    // Bind an ephemeral port, then close it without ever listening -- the
    // OS is guaranteed to refuse connections to it immediately afterwards.
    int probeFd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(probeFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    sockaddr_in bound{};
    socklen_t boundLen = sizeof(bound);
    ::getsockname(probeFd, reinterpret_cast<sockaddr*>(&bound), &boundLen);
    int closedPort = ntohs(bound.sin_port);
    ::close(probeFd);

    std::string error;
    std::unique_ptr<TcpTransport> transport =
        TcpTransport::connectTo("127.0.0.1", closedPort, &error);
    CHECK(transport == nullptr);
    CHECK(!error.empty());
}

TEST(TcpTransportSuite, writeAfterPeerClosesFailsWithoutKillingProcess) {
    int port = 0;
    int listenFd = startListener(&port);

    std::thread server([listenFd]() {
        int connFd = ::accept(listenFd, nullptr, nullptr);
        ::close(connFd);
    });

    std::string error;
    std::unique_ptr<TcpTransport> transport = TcpTransport::connectTo("127.0.0.1", port, &error);
    CHECK(transport != nullptr);

    // Make sure the peer has actually closed before we start writing.
    server.join();
    ::close(listenFd);

    std::string chunk(4096, 'a');
    bool sawFailure = false;
    for (int attempt = 0; attempt < 200 && !sawFailure; ++attempt) {
        if (!transport->writeAll(chunk.data(), chunk.size())) {
            sawFailure = true;
        }
    }

    // If SIGPIPE were not suppressed, the process would already be dead and
    // this assertion would never run -- reaching it at all is part of the
    // proof.
    CHECK(sawFailure);
    CHECK(!transport->lastError().empty());
}

TEST(TcpTransportSuite, readSomeSetsLastErrorOnARealErrorPath) {
    int port = 0;
    int listenFd = startListener(&port);

    std::thread server([listenFd]() {
        int connFd = ::accept(listenFd, nullptr, nullptr);
        ::close(connFd);
    });

    std::string error;
    std::unique_ptr<TcpTransport> transport = TcpTransport::connectTo("127.0.0.1", port, &error);
    CHECK(transport != nullptr);

    server.join();
    ::close(listenFd);

    // Force a real, non-EINTR error out of readSome: closing the transport
    // drops fd_ to -1, so the next recv() fails with EBADF. Proves
    // lastError() is never left stale on a path that returns -1.
    transport->close();
    char buf[1];
    ptrdiff_t result = transport->readSome(buf, sizeof(buf));

    CHECK_EQ(result, -1);
    CHECK(!transport->lastError().empty());
}

// ---------------------------------------------------------------------
// TcpTransport's socket timeouts, end to end over a real socket against a
// deliberately silent peer. connectTo takes the timeout in milliseconds so
// these can prove the behaviour in well under a second instead of waiting
// out the SOCKET_TIMEOUT_MS the plugin itself uses.
// ---------------------------------------------------------------------

TEST(TcpTransportSuite, stalledReadPollsTheStallCallbackAndReportsAnErrorWhenCancelled) {
    int port = 0;
    int listenFd = startListener(&port);

    // The server accepts and then says nothing at all until the client is
    // done -- exactly what a sleeping phone looks like mid-transfer.
    std::thread server([listenFd]() {
        int connFd = ::accept(listenFd, nullptr, nullptr);
        char sink[1];
        ::recv(connFd, sink, sizeof(sink), 0);
        ::close(connFd);
    });

    std::string error;
    std::unique_ptr<TcpTransport> transport =
        TcpTransport::connectTo("127.0.0.1", port, &error, /*timeoutMs=*/50);
    CHECK(transport != nullptr);

    int stallPolls = 0;
    transport->setStallCallback([&stallPolls]() -> bool {
        ++stallPolls;
        return stallPolls < 3; // keep waiting twice, then "Cancel"
    });

    char buf[16];
    ptrdiff_t result = transport->readSome(buf, sizeof(buf));

    CHECK_EQ(result, -1); // an abandoned stall is an error, NOT EOF
    CHECK_EQ(stallPolls, 3);
    CHECK(transport->lastError().find("cancel") != std::string::npos);

    transport->close();
    server.join();
    ::close(listenFd);
}

TEST(TcpTransportSuite, stalledReadResumesWhenDataFinallyArrives) {
    int port = 0;
    int listenFd = startListener(&port);

    std::thread server([listenFd]() {
        int connFd = ::accept(listenFd, nullptr, nullptr);
        // Stay silent long enough to trip several 50 ms read timeouts,
        // then deliver -- the transport must not have given up.
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        ::send(connFd, "late", 4, 0);
        char sink[1];
        ::recv(connFd, sink, sizeof(sink), 0);
        ::close(connFd);
    });

    std::string error;
    std::unique_ptr<TcpTransport> transport =
        TcpTransport::connectTo("127.0.0.1", port, &error, /*timeoutMs=*/50);
    CHECK(transport != nullptr);

    int stallPolls = 0;
    transport->setStallCallback([&stallPolls]() -> bool {
        ++stallPolls;
        return true; // never cancelled
    });

    char buf[4] = {};
    CHECK(transport->readExactly(buf, 4));
    CHECK(std::memcmp(buf, "late", 4) == 0);
    CHECK(stallPolls > 0); // the stall path really was exercised

    transport->close();
    server.join();
    ::close(listenFd);
}

// The polarity that shipped a hang. readSome consults the stall callback
// itself, but readExactly drives readSome through loopTransfer -- and
// loopTransfer was called WITHOUT the stall callback, so `onStall` was
// null inside the loop. A readSome that returned -1/EAGAIN *because the
// user cancelled* then hit loopTransfer's timeout branch, where
// stallShouldKeepWaiting(nullptr) is true, and the loop went straight
// back into readSome to stall and cancel again, forever. syncRecv reads
// every sync header and every DATA chunk through readExactly, so Cancel
// on a stalled pull hung the Double Commander worker thread outright.
//
// A FakeTransport cannot see this: its readExactly is a hand-rolled loop
// that never touches loopTransfer. Only a real TcpTransport can, which is
// why this test drives one against a silent peer. It runs the read on its
// own thread with a deadline so the regression fails the suite instead of
// hanging it.

TEST(TcpTransportSuite, stalledReadExactlyFailsPromptlyWhenCancelledRatherThanLooping) {
    int port = 0;
    int listenFd = startListener(&port);

    // Accepts, then says nothing until the test releases it. Closing at
    // the end is what lets a REGRESSED build's spinning reader terminate,
    // so a failure here is a failed assertion and not a stuck suite.
    std::atomic<bool> release{false};
    std::thread server([listenFd, &release]() {
        int connFd = ::accept(listenFd, nullptr, nullptr);
        while (!release) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ::close(connFd);
    });

    std::string error;
    std::unique_ptr<TcpTransport> transport =
        TcpTransport::connectTo("127.0.0.1", port, &error, /*timeoutMs=*/50);
    CHECK(transport != nullptr);

    std::atomic<int> stallPolls{0};
    transport->setStallCallback([&stallPolls]() -> bool {
        ++stallPolls;
        return false; // "Cancel", every single time it is asked
    });

    std::promise<bool> readResult;
    std::future<bool> readFuture = readResult.get_future();
    std::thread reader([&]() {
        char buf[16];
        readResult.set_value(transport->readExactly(buf, sizeof(buf)));
    });

    bool finished = readFuture.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    CHECK(finished);
    if (finished) {
        CHECK(!readFuture.get()); // a cancelled read must fail, not succeed
    }
    // Two polls: readSome's own retryOnEintr asks once, then loopTransfer
    // asks once before giving up. The regression produced dozens.
    CHECK(stallPolls <= 4);

    release = true;
    reader.join();
    transport->close();
    server.join();
    ::close(listenFd);
}
