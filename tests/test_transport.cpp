// Tests for transport.hpp (the I/O seam) and tests/fake_transport.hpp (the
// scripted double Tasks 6-9 are built against). Written before either
// header existed (TDD) -- see
// .superpowers/sdd/plan-adb-wfx/task-5-report.md for the RED run.
#include "../transport.hpp"
#include "fake_transport.hpp"
#include "testing.hpp"

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

