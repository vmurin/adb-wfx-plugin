// Tests for adbclient.hpp: the ADB client that drives the wire protocol
// (host requests, sync v1, shell:) over the Transport seam. Written before
// adbclient.hpp exists (TDD) -- see
// .superpowers/sdd/plan-adb-wfx/task-7-report.md for the RED run. Every
// test here is driven by a scripted FakeTransport; nothing touches a real
// adb server or phone.
#include "adbclient.hpp"
#include "fake_transport.hpp"
#include "testing.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

// ---------------------------------------------------------------------
// Test fixtures
// ---------------------------------------------------------------------

// A FakeTransport that, on destruction, copies its written() bytes and
// closed state into caller-owned storage. AdbClient owns the transport it
// gets from the factory in a local std::unique_ptr and destroys it before
// control returns to the test, so this is the only way to inspect what was
// written (or whether close() was called) once the call has returned.
class RecordingTransport : public FakeTransport {
public:
    RecordingTransport(const std::string& scriptedReplies, std::string* writtenOut,
                        bool* closedOut)
        : FakeTransport(scriptedReplies), writtenOut_(writtenOut), closedOut_(closedOut) {}

    ~RecordingTransport() override {
        if (writtenOut_ != nullptr) {
            *writtenOut_ = written();
        }
        if (closedOut_ != nullptr) {
            *closedOut_ = isClosed();
        }
    }

private:
    std::string* writtenOut_;
    bool* closedOut_;
};

// A FakeTransport that stalls before handing over its scripted bytes --
// the double for a phone that goes to sleep mid-transfer. Each of the
// first `stalls` readSome calls consults the stall callback AdbClient
// installed (Transport::setStallCallback) exactly as TcpTransport does
// when SO_RCVTIMEO fires: if it says keep waiting, the read proceeds; if
// it says stop, the read fails with a timeout errno, which is what a
// real cancelled stall looks like from the caller's side.
class StallingTransport : public FakeTransport {
public:
    // stallPollsOut, like RecordingTransport's outputs above, is written
    // on destruction: AdbClient destroys the transport before returning,
    // so a test cannot read anything off the object itself afterwards.
    StallingTransport(const std::string& scriptedReplies, int stalls, int* stallPollsOut)
        : FakeTransport(scriptedReplies), stallsLeft_(stalls), stallPollsOut_(stallPollsOut) {}

    ~StallingTransport() override {
        if (stallPollsOut_ != nullptr) {
            *stallPollsOut_ = stallPolls_;
        }
    }

    ptrdiff_t readSome(void* buf, size_t n) override {
        // Only stalls once a stall callback is installed, i.e. once the
        // host:transport:/sync: handshake is done and the transfer proper
        // has begun -- which is the only window AdbClient arms it for.
        const StallFn& onStall = stallCallback();
        if (stallsLeft_ > 0 && onStall) {
            --stallsLeft_;
            ++stallPolls_;
            if (!onStall()) {
                errno = EAGAIN;
                return -1;
            }
        }
        return FakeTransport::readSome(buf, n);
    }

private:
    int stallsLeft_;
    int* stallPollsOut_;
    int stallPolls_ = 0;
};

// Wraps a single transport in a TransportFactory that hands it out exactly
// once, matching AdbClient's "fresh transport per operation" contract. A
// second call (which would indicate a bug in AdbClient) fails loudly
// instead of silently returning nullptr.
TransportFactory singleUseFactory(std::unique_ptr<Transport> transport) {
    auto slot = std::make_shared<std::unique_ptr<Transport>>(std::move(transport));
    return [slot](std::string* err) -> std::unique_ptr<Transport> {
        if (*slot) {
            return std::move(*slot);
        }
        if (err != nullptr) {
            *err = "test factory: transport already used";
        }
        return nullptr;
    };
}

// A temp file (auto-removed on close) that gives AdbClient an already-open
// fd to read from or write into, per the task's contract that AdbClient
// never opens/creates/deletes local files itself.
class TempFile {
public:
    TempFile() : fp_(std::tmpfile()) {
        CHECK(fp_ != nullptr);
    }
    ~TempFile() {
        if (fp_ != nullptr) {
            std::fclose(fp_);
        }
    }
    int fd() const { return fp_ != nullptr ? fileno(fp_) : -1; }

private:
    FILE* fp_;
};

// Writes the whole of `content` starting at offset 0. A short or failed
// write is a broken test fixture, not a scenario under test -- fail loudly
// here rather than silently leaving less data in the file than the caller
// asked for and surfacing as a baffling content mismatch somewhere else.
void writeWholeFile(int fd, const std::string& content) {
    ::lseek(fd, 0, SEEK_SET);
    size_t done = 0;
    while (done < content.size()) {
        ssize_t n = ::write(fd, content.data() + done, content.size() - done);
        if (n <= 0) {
            break;
        }
        done += static_cast<size_t>(n);
    }
    CHECK_EQ(done, content.size());
    ::lseek(fd, 0, SEEK_SET);
}

std::string readWholeFile(int fd) {
    ::lseek(fd, 0, SEEK_SET);
    std::string result;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
        result.append(buf, static_cast<size_t>(n));
    }
    return result;
}

// ---------------------------------------------------------------------
// Wire-format helpers built on top of adbproto.hpp's codec, used only to
// script FakeTransport replies and to compute the expected written() bytes.
// ---------------------------------------------------------------------

std::string hexLen4(size_t n) {
    char buf[5];
    std::snprintf(buf, sizeof(buf), "%04x", static_cast<unsigned int>(n));
    return std::string(buf, 4);
}

std::string syncHeaderBytes(const char* id, uint32_t arg) {
    std::array<unsigned char, 8> header = encodeSyncHeader(id, arg);
    return std::string(reinterpret_cast<char*>(header.data()), header.size());
}

// A sync-protocol FAIL packet: 8-byte header (id="FAIL", arg=message
// length) followed by the message bytes -- used for RECV/SEND/LIST-stream
// errors (never the hex-ASCII length used by host-level FAIL).
std::string syncFailBytes(const std::string& message) {
    return syncHeaderBytes("FAIL", static_cast<uint32_t>(message.size())) + message;
}

// A DENT entry: id + mode + size + mtime + namelen (each u32 LE) + name.
std::string encodeDent(uint32_t mode, uint32_t size, uint32_t mtime, const std::string& name) {
    std::string s = "DENT";
    unsigned char buf[4];
    writeU32Le(buf, mode);
    s.append(reinterpret_cast<char*>(buf), 4);
    writeU32Le(buf, size);
    s.append(reinterpret_cast<char*>(buf), 4);
    writeU32Le(buf, mtime);
    s.append(reinterpret_cast<char*>(buf), 4);
    writeU32Le(buf, static_cast<uint32_t>(name.size()));
    s.append(reinterpret_cast<char*>(buf), 4);
    s += name;
    return s;
}

// LIST's terminal packet: "DONE" + 16 zero bytes (not the generic 8-byte
// header -- see constraints.md's sync protocol reference).
std::string encodeListDone() {
    return std::string("DONE") + std::string(16, '\0');
}

// A DENT's fixed 16-byte header (mode + size + mtime + namelen) with no
// name bytes following -- used to script a hostile namelen without having
// to actually put that many bytes in the script. A correct implementation
// must reject this before ever trying to read (or allocate for) the name.
std::string encodeDentFixedHeader(uint32_t mode, uint32_t size, uint32_t mtime, uint32_t namelen) {
    std::string s = "DENT";
    unsigned char buf[4];
    writeU32Le(buf, mode);
    s.append(reinterpret_cast<char*>(buf), 4);
    writeU32Le(buf, size);
    s.append(reinterpret_cast<char*>(buf), 4);
    writeU32Le(buf, mtime);
    s.append(reinterpret_cast<char*>(buf), 4);
    writeU32Le(buf, namelen);
    s.append(reinterpret_cast<char*>(buf), 4);
    return s;
}

std::string encodeStatBody(uint32_t mode, uint32_t size, uint32_t mtime) {
    std::string s;
    unsigned char buf[4];
    writeU32Le(buf, mode);
    s.append(reinterpret_cast<char*>(buf), 4);
    writeU32Le(buf, size);
    s.append(reinterpret_cast<char*>(buf), 4);
    writeU32Le(buf, mtime);
    s.append(reinterpret_cast<char*>(buf), 4);
    return s;
}

} // namespace

// ---------------------------------------------------------------------
// listDevices
// ---------------------------------------------------------------------

TEST(AdbClientSuite, listDevicesHappyPathWritesExactBytesAndParsesDevices) {
    std::string devicesText =
        "SERIAL1\tdevice\n"
        "SERIAL2          device usb:1-1 product:panther model:Pixel_7 device:panther "
        "transport_id:5\n";
    std::string script = "OKAY" + hexLen4(devicesText.size()) + devicesText;

    std::string written;
    bool closed = false;
    auto transport = std::make_unique<RecordingTransport>(script, &written, &closed);
    AdbClient client(singleUseFactory(std::move(transport)));

    std::vector<DeviceInfo> devices;
    AdbError err = client.listDevices(&devices);

    CHECK(err.ok);
    CHECK_STR_EQ(written, std::string("000ehost:devices-l"));
    CHECK(closed);
    CHECK_EQ(devices.size(), static_cast<size_t>(2));
    CHECK_STR_EQ(devices[0].serial, "SERIAL1");
    CHECK_STR_EQ(devices[0].state, "device");
    CHECK_STR_EQ(devices[0].model, "");
    CHECK_STR_EQ(devices[1].serial, "SERIAL2");
    CHECK_STR_EQ(devices[1].state, "device");
    CHECK_STR_EQ(devices[1].model, "Pixel_7");
}

TEST(AdbClientSuite, listDevicesFailSurfacesServerMessage) {
    std::string message = "no adb server running";
    std::string script = "FAIL" + hexLen4(message.size()) + message;
    auto transport = std::make_unique<RecordingTransport>(script, nullptr, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));

    std::vector<DeviceInfo> devices;
    AdbError err = client.listDevices(&devices);

    CHECK(!err.ok);
    CHECK_STR_EQ(err.message, message);
    CHECK(devices.empty());
}

// ---------------------------------------------------------------------
// syncList
// ---------------------------------------------------------------------

TEST(AdbClientSuite, syncListHappyPathWritesFramedRequestsAndParsesEntries) {
    std::string serial = "SERIAL1";
    std::string path = "/sdcard/DCIM";

    std::string dent1 = encodeDent(0100644, 1234, 1600000000, "a.jpg");
    std::string dent2 = encodeDent(0040755, 0, 1600000001, "sub");
    std::string script = "OKAY" "OKAY" + dent1 + dent2 + encodeListDone();

    std::string written;
    bool closed = false;
    auto transport = std::make_unique<RecordingTransport>(script, &written, &closed);
    AdbClient client(singleUseFactory(std::move(transport)));

    std::vector<DirEntry> entries;
    AdbError err = client.syncList(serial, path, &entries);

    CHECK(err.ok);
    CHECK(closed);

    std::string expectedWritten = encodeHostRequest("host:transport:" + serial) +
                                   encodeHostRequest("sync:") +
                                   syncHeaderBytes("LIST", static_cast<uint32_t>(path.size())) +
                                   path;
    CHECK_STR_EQ(written, expectedWritten);

    CHECK_EQ(entries.size(), static_cast<size_t>(2));
    CHECK_STR_EQ(entries[0].name, "a.jpg");
    CHECK_EQ(entries[0].mode, static_cast<uint32_t>(0100644));
    CHECK_EQ(entries[0].size, static_cast<uint64_t>(1234));
    CHECK_EQ(entries[0].mtime, static_cast<int64_t>(1600000000));
    CHECK(!entries[0].isDir());
    CHECK_STR_EQ(entries[1].name, "sub");
    CHECK(entries[1].isDir());
}

TEST(AdbClientSuite, syncListFailInsideSyncStreamSurfacesMessage) {
    std::string message = "permission denied";
    std::string script = "OKAY" "OKAY" + syncFailBytes(message);
    auto transport = std::make_unique<RecordingTransport>(script, nullptr, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));

    std::vector<DirEntry> entries;
    AdbError err = client.syncList("SERIAL1", "/nope", &entries);

    CHECK(!err.ok);
    CHECK_STR_EQ(err.message, message);
    CHECK(entries.empty());
}

// ---------------------------------------------------------------------
// Hostile wire lengths must fail cleanly, not attempt a huge allocation
// ---------------------------------------------------------------------

TEST(AdbClientSuite, syncListRejectsHostileDentNamelenWithoutHugeAllocation) {
    // namelen = 0xFFFFFFFF, with no name bytes following in the script at
    // all -- a correct implementation rejects this from the fixed 16-byte
    // header alone, before ever trying to read (or size an allocation for)
    // the name.
    std::string script =
        "OKAY" "OKAY" + encodeDentFixedHeader(0100644, 0, 0, 0xFFFFFFFF);
    auto transport = std::make_unique<RecordingTransport>(script, nullptr, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));

    std::vector<DirEntry> entries;
    AdbError err = client.syncList("SERIAL1", "/sdcard", &entries);

    CHECK(!err.ok);
    CHECK(!err.message.empty());
    CHECK(entries.empty());
}

TEST(AdbClientSuite, syncRecvRejectsHostileFailLengthWithoutHugeAllocation) {
    // A FAIL header claiming a 0xFFFFFFFF-byte message, with no message
    // bytes following in the script -- must be rejected from the header
    // alone, before ever trying to read (or size an allocation for) the
    // message.
    std::string script = "OKAY" "OKAY" + syncHeaderBytes("FAIL", 0xFFFFFFFF);
    auto transport = std::make_unique<RecordingTransport>(script, nullptr, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));

    TempFile temp;
    ProgressFn progress = [](uint64_t, uint64_t) { return true; };
    AdbError err = client.syncRecv("SERIAL1", "/sdcard/f.bin", temp.fd(), 0, progress);

    CHECK(!err.ok);
    CHECK(!err.message.empty());
}

// ---------------------------------------------------------------------
// syncStat
// ---------------------------------------------------------------------

TEST(AdbClientSuite, syncStatExistingFileReturnsFields) {
    std::string script = "OKAY" "OKAY" "STAT" + encodeStatBody(0100644, 42, 1600000005);
    auto transport = std::make_unique<RecordingTransport>(script, nullptr, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));

    DirEntry entry;
    bool exists = false;
    AdbError err = client.syncStat("SERIAL1", "/sdcard/a.txt", &entry, &exists);

    CHECK(err.ok);
    CHECK(exists);
    CHECK_EQ(entry.mode, static_cast<uint32_t>(0100644));
    CHECK_EQ(entry.size, static_cast<uint64_t>(42));
    CHECK_EQ(entry.mtime, static_cast<int64_t>(1600000005));
}

TEST(AdbClientSuite, syncStatNonExistentFileReturnsExistsFalse) {
    std::string script = "OKAY" "OKAY" "STAT" + encodeStatBody(0, 0, 0);
    auto transport = std::make_unique<RecordingTransport>(script, nullptr, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));

    DirEntry entry;
    bool exists = true; // start true to prove syncStat actually resets it
    AdbError err = client.syncStat("SERIAL1", "/sdcard/missing.txt", &entry, &exists);

    CHECK(err.ok);
    CHECK(!exists);
}

// ---------------------------------------------------------------------
// syncRecv
// ---------------------------------------------------------------------

TEST(AdbClientSuite, syncRecvWritesFileContentsAndCallsProgress) {
    std::string chunk1(40000, 'A');
    std::string chunk2(20000, 'B');
    std::string script = "OKAY" "OKAY" + syncHeaderBytes("DATA", static_cast<uint32_t>(chunk1.size())) +
                          chunk1 + syncHeaderBytes("DATA", static_cast<uint32_t>(chunk2.size())) +
                          chunk2 + syncHeaderBytes("DONE", 0);

    auto transport = std::make_unique<RecordingTransport>(script, nullptr, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));

    TempFile temp;
    std::vector<std::pair<uint64_t, uint64_t>> progressCalls;
    ProgressFn progress = [&](uint64_t done, uint64_t total) {
        progressCalls.push_back({done, total});
        return true;
    };

    AdbError err = client.syncRecv("SERIAL1", "/sdcard/f.bin", temp.fd(), 60000, progress);

    CHECK(err.ok);
    CHECK_EQ(progressCalls.size(), static_cast<size_t>(2));
    CHECK_EQ(progressCalls[0].first, static_cast<uint64_t>(40000));
    CHECK_EQ(progressCalls[0].second, static_cast<uint64_t>(60000));
    CHECK_EQ(progressCalls[1].first, static_cast<uint64_t>(60000));
    CHECK_EQ(progressCalls[1].second, static_cast<uint64_t>(60000));

    std::string fileContents = readWholeFile(temp.fd());
    CHECK_EQ(fileContents.size(), static_cast<size_t>(60000));
    CHECK_STR_EQ(fileContents, chunk1 + chunk2);
}

TEST(AdbClientSuite, syncRecvCancellationStopsAfterFirstChunkWithoutReportingSuccess) {
    std::string chunk1(1000, 'A');
    std::string chunk2(1000, 'B');
    std::string script = "OKAY" "OKAY" + syncHeaderBytes("DATA", static_cast<uint32_t>(chunk1.size())) +
                          chunk1 + syncHeaderBytes("DATA", static_cast<uint32_t>(chunk2.size())) +
                          chunk2 + syncHeaderBytes("DONE", 0);

    bool closed = false;
    auto transport = std::make_unique<RecordingTransport>(script, nullptr, &closed);
    AdbClient client(singleUseFactory(std::move(transport)));

    TempFile temp;
    int progressCalls = 0;
    ProgressFn progress = [&](uint64_t, uint64_t) {
        ++progressCalls;
        return false;
    };

    AdbError err = client.syncRecv("SERIAL1", "/sdcard/f.bin", temp.fd(), 2000, progress);

    CHECK(!err.ok);
    CHECK_STR_EQ(err.message, std::string(ADB_CANCELLED));
    CHECK_EQ(progressCalls, 1);
    CHECK(closed);

    // Only the first chunk was ever read and written -- the second DATA
    // packet must not have been touched, and the cancellation must not be
    // silently reported as success.
    std::string fileContents = readWholeFile(temp.fd());
    CHECK_STR_EQ(fileContents, chunk1);
}

TEST(AdbClientSuite, syncRecvEofMidDataFails) {
    // The header announces a 1000-byte chunk, but only 200 bytes actually
    // follow before the scripted stream runs out.
    std::string script = "OKAY" "OKAY" + syncHeaderBytes("DATA", 1000) + std::string(200, 'A');
    auto transport = std::make_unique<RecordingTransport>(script, nullptr, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));

    TempFile temp;
    ProgressFn progress = [](uint64_t, uint64_t) { return true; };
    AdbError err = client.syncRecv("SERIAL1", "/sdcard/f.bin", temp.fd(), 1000, progress);

    CHECK(!err.ok);
    CHECK(!err.message.empty());
    // The chunk is read whole before any of it is written to the local
    // file, so a short DATA chunk must leave nothing behind -- not the 200
    // bytes that did arrive.
    CHECK(readWholeFile(temp.fd()).empty());
}

TEST(AdbClientSuite, syncRecvFailSurfacesServerMessage) {
    std::string message = "No such file or directory";
    std::string script = "OKAY" "OKAY" + syncFailBytes(message);
    auto transport = std::make_unique<RecordingTransport>(script, nullptr, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));

    TempFile temp;
    ProgressFn progress = [](uint64_t, uint64_t) { return true; };
    AdbError err = client.syncRecv("SERIAL1", "/sdcard/missing", temp.fd(), 0, progress);

    CHECK(!err.ok);
    CHECK_STR_EQ(err.message, message);
}

// ---------------------------------------------------------------------
// syncSend
// ---------------------------------------------------------------------

TEST(AdbClientSuite, syncSendSplitsIntoExactChunksAndPreservesMtime) {
    std::string serial = "SERIAL1";
    std::string remote = "/sdcard/big.bin";
    uint32_t mode = 0100644;
    int64_t mtime = 1700000000;

    const size_t fileSize = 150000;
    std::string content;
    content.reserve(fileSize);
    for (size_t i = 0; i < fileSize; ++i) {
        content.push_back(static_cast<char>('A' + (i % 26)));
    }

    TempFile temp;
    writeWholeFile(temp.fd(), content);

    std::string script = "OKAY" "OKAY" + syncHeaderBytes("OKAY", 0);
    std::string written;
    auto transport = std::make_unique<RecordingTransport>(script, &written, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));

    ProgressFn progress = [](uint64_t, uint64_t) { return true; };
    AdbError err = client.syncSend(serial, temp.fd(), fileSize, remote, mode, mtime, progress);

    CHECK(err.ok);

    size_t offset = 0;
    std::string hostTransportReq = encodeHostRequest("host:transport:" + serial);
    CHECK_EQ(written.compare(offset, hostTransportReq.size(), hostTransportReq), 0);
    offset += hostTransportReq.size();

    std::string syncReq = encodeHostRequest("sync:");
    CHECK_EQ(written.compare(offset, syncReq.size(), syncReq), 0);
    offset += syncReq.size();

    std::string spec = encodeSendPathSpec(remote, mode);
    std::string sendHeader = syncHeaderBytes("SEND", static_cast<uint32_t>(spec.size()));
    CHECK_EQ(written.compare(offset, sendHeader.size(), sendHeader), 0);
    offset += sendHeader.size();
    CHECK_EQ(written.compare(offset, spec.size(), spec), 0);
    offset += spec.size();

    // The whole point of this project: a 150000-byte file is exactly
    // 65536 + 65536 + 18928, and the final DONE's arg must be the mtime.
    std::vector<uint32_t> expectedChunkSizes = {65536, 65536, 18928};
    size_t contentOffset = 0;
    for (uint32_t chunkSize : expectedChunkSizes) {
        std::string dataHeader = syncHeaderBytes("DATA", chunkSize);
        CHECK_EQ(written.compare(offset, dataHeader.size(), dataHeader), 0);
        offset += dataHeader.size();
        std::string expectedChunk = content.substr(contentOffset, chunkSize);
        CHECK_EQ(written.compare(offset, chunkSize, expectedChunk), 0);
        offset += chunkSize;
        contentOffset += chunkSize;
    }

    std::string doneHeader = syncHeaderBytes("DONE", static_cast<uint32_t>(mtime));
    CHECK_EQ(written.compare(offset, doneHeader.size(), doneHeader), 0);

    // This is the headline criterion of the entire project: preserving
    // modification times. Pin it a second time, independently of
    // encodeSyncHeader (the same function the production code uses to build
    // this very header), by decoding the id and the little-endian arg by
    // hand straight out of the written byte stream.
    CHECK_EQ(std::memcmp(written.data() + offset, "DONE", 4), 0);
    CHECK_EQ(readU32Le(reinterpret_cast<const unsigned char*>(written.data()) + offset + 4),
             static_cast<uint32_t>(mtime));

    offset += doneHeader.size();

    CHECK_EQ(offset, written.size());
}

TEST(AdbClientSuite, syncSendCancellationStopsMidFileWithoutReportingSuccess) {
    const size_t fileSize = 150000;
    std::string content(fileSize, 'Z');
    TempFile temp;
    writeWholeFile(temp.fd(), content);

    std::string remote = "/sdcard/big.bin";
    uint32_t mode = 0100644;

    // No final reply scripted -- a canceled send must never read it.
    std::string script = "OKAY" "OKAY";
    std::string written;
    bool closed = false;
    auto transport = std::make_unique<RecordingTransport>(script, &written, &closed);
    AdbClient client(singleUseFactory(std::move(transport)));

    int progressCalls = 0;
    ProgressFn progress = [&](uint64_t, uint64_t) {
        ++progressCalls;
        return false; // cancel right after the first chunk
    };

    AdbError err = client.syncSend("SERIAL1", temp.fd(), fileSize, remote, mode, 1700000000, progress);

    CHECK(!err.ok);
    CHECK_STR_EQ(err.message, std::string(ADB_CANCELLED));
    CHECK_EQ(progressCalls, 1);
    CHECK(closed);

    std::string spec = encodeSendPathSpec(remote, mode);
    size_t expectedLen = encodeHostRequest("host:transport:SERIAL1").size() +
                          encodeHostRequest("sync:").size() +
                          syncHeaderBytes("SEND", static_cast<uint32_t>(spec.size())).size() +
                          spec.size() + syncHeaderBytes("DATA", 65536).size() + 65536;
    CHECK_EQ(written.size(), expectedLen);
}

TEST(AdbClientSuite, syncSendFailAfterDoneSurfacesServerMessage) {
    std::string content(100, 'X');
    TempFile temp;
    writeWholeFile(temp.fd(), content);

    std::string message = "No space left on device";
    std::string script = "OKAY" "OKAY" + syncFailBytes(message);
    auto transport = std::make_unique<RecordingTransport>(script, nullptr, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));

    ProgressFn progress = [](uint64_t, uint64_t) { return true; };
    AdbError err = client.syncSend("SERIAL1", temp.fd(), content.size(), "/sdcard/f.bin", 0100644,
                                    1700000000, progress);

    CHECK(!err.ok);
    CHECK_STR_EQ(err.message, message);
}

// When adbd hits a write error partway through a SEND -- the card fills
// up at 40% of a 2 GB video, the target directory is read-only -- it
// answers with a sync FAIL and closes. The client is several 64 KB chunks
// ahead by then, so what it notices is its own write failing with EPIPE,
// while "No space left on device" sits unread in the socket buffer and
// used to be thrown away by t->close(). "failed to write DATA chunk:
// Broken pipe" is true and useless; "your phone is full" is the one thing
// a file manager has to get right here.

TEST(AdbClientSuite, syncSendPrefersAPendingFailMessageOverTheWriteErrno) {
    const size_t fileSize = 150000; // three DATA chunks
    std::string content(fileSize, 'Q');
    TempFile temp;
    writeWholeFile(temp.fd(), content);

    std::string remote = "/sdcard/big.bin";
    uint32_t mode = 0100644;
    std::string message = "No space left on device";

    std::string script = "OKAY" "OKAY" + syncFailBytes(message);
    auto transport = std::make_unique<RecordingTransport>(script, nullptr, nullptr);
    // Fail the write partway through the second DATA chunk: the handshake
    // and the first chunk get through, then the socket goes.
    std::string spec = encodeSendPathSpec(remote, mode);
    size_t throughFirstChunk = encodeHostRequest("host:transport:SERIAL1").size() +
                                encodeHostRequest("sync:").size() +
                                syncHeaderBytes("SEND", static_cast<uint32_t>(spec.size())).size() +
                                spec.size() + syncHeaderBytes("DATA", 65536).size() + 65536;
    transport->failWritesAfterBytes(throughFirstChunk);
    AdbClient client(singleUseFactory(std::move(transport)));

    ProgressFn progress = [](uint64_t, uint64_t) { return true; };
    AdbError err =
        client.syncSend("SERIAL1", temp.fd(), fileSize, remote, mode, 1700000000, progress);

    CHECK(!err.ok);
    CHECK_STR_EQ(err.message, message);
}

TEST(AdbClientSuite, syncSendKeepsTheWriteErrorWhenNoFailPacketIsWaiting) {
    const size_t fileSize = 150000;
    std::string content(fileSize, 'Q');
    TempFile temp;
    writeWholeFile(temp.fd(), content);

    std::string remote = "/sdcard/big.bin";
    uint32_t mode = 0100644;

    // Nothing scripted past the handshake: the peer went away without
    // saying anything, so the transport's own error text is all there is.
    auto transport = std::make_unique<RecordingTransport>(std::string("OKAY" "OKAY"), nullptr, nullptr);
    std::string spec = encodeSendPathSpec(remote, mode);
    size_t throughFirstChunk = encodeHostRequest("host:transport:SERIAL1").size() +
                                encodeHostRequest("sync:").size() +
                                syncHeaderBytes("SEND", static_cast<uint32_t>(spec.size())).size() +
                                spec.size() + syncHeaderBytes("DATA", 65536).size() + 65536;
    transport->failWritesAfterBytes(throughFirstChunk);
    AdbClient client(singleUseFactory(std::move(transport)));

    ProgressFn progress = [](uint64_t, uint64_t) { return true; };
    AdbError err =
        client.syncSend("SERIAL1", temp.fd(), fileSize, remote, mode, 1700000000, progress);

    CHECK(!err.ok);
    CHECK(err.message.find("failed to write DATA chunk") != std::string::npos);
}

TEST(AdbClientSuite, shellCommandRefusesACommandTooLongToFrame) {
    // Unlike the sync services, shell: has no SYNC_PATH_MAX guard ahead
    // of it, so an absurdly long path arriving from Double Commander
    // really can produce a service string past the four-hex-digit length
    // prefix. It must fail cleanly rather than write a truncated frame
    // the server would parse as two requests.
    std::string written;
    auto transport = std::make_unique<RecordingTransport>(std::string("OKAY"), &written, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));

    std::string output;
    AdbError err = client.shellCommand("SERIAL1", std::string(70000, 'x'), &output);

    CHECK(!err.ok);
    CHECK(err.message.find("too long") != std::string::npos);
    // Only the host:transport: handshake went out -- no partial frame.
    CHECK_STR_EQ(written, encodeHostRequest("host:transport:SERIAL1"));
}

// ---------------------------------------------------------------------
// Cancellation while the socket is stalled.
//
// The between-chunks cancellation check is useless when the bytes stop
// arriving: a sleeping phone parks the caller inside one recv() and the
// next chunk boundary never comes. syncRecv/syncSend therefore install a
// stall callback on the transport (transport.hpp) that re-polls the
// progress callback each time the socket times out, and a stall the user
// gave up on must be reported as a cancellation -- not as an I/O error
// for a button they pressed themselves.
// ---------------------------------------------------------------------

TEST(AdbClientSuite, syncRecvPollsProgressWhileStalledAndKeepsGoingWhenNotCancelled) {
    std::string content = "arrived after a stall";
    std::string script = "OKAY" "OKAY" +
                          syncHeaderBytes("DATA", static_cast<uint32_t>(content.size())) + content +
                          syncHeaderBytes("DONE", 0);

    int stallPolls = 0;
    auto transport = std::make_unique<StallingTransport>(script, /*stalls=*/3, &stallPolls);
    AdbClient client(singleUseFactory(std::move(transport)));

    TempFile temp;
    int progressCalls = 0;
    ProgressFn progress = [&](uint64_t, uint64_t) {
        ++progressCalls;
        return true; // the user has not cancelled
    };

    AdbError err = client.syncRecv("SERIAL1", "/sdcard/f.bin", temp.fd(), content.size(), progress);

    CHECK(err.ok);
    CHECK_EQ(stallPolls, 3);
    CHECK(progressCalls >= 3); // the stalls polled it, over and above the chunk boundary
    CHECK_STR_EQ(readWholeFile(temp.fd()), content);
}

TEST(AdbClientSuite, syncRecvCancelledDuringAStallReportsCancellationNotAnIoError) {
    std::string content = "never delivered";
    std::string script = "OKAY" "OKAY" +
                          syncHeaderBytes("DATA", static_cast<uint32_t>(content.size())) + content +
                          syncHeaderBytes("DONE", 0);

    auto transport = std::make_unique<StallingTransport>(script, /*stalls=*/1, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));

    TempFile temp;
    ProgressFn progress = [](uint64_t, uint64_t) {
        return false; // Cancel, pressed while nothing is moving
    };

    AdbError err = client.syncRecv("SERIAL1", "/sdcard/f.bin", temp.fd(), content.size(), progress);

    CHECK(!err.ok);
    CHECK_STR_EQ(err.message, std::string(ADB_CANCELLED));
}

// ---------------------------------------------------------------------
// shellCommand
// ---------------------------------------------------------------------

TEST(AdbClientSuite, shellCommandReadsUntilEofAndReturnsOutput) {
    std::string serial = "SERIAL1";
    std::string command = "ls -la /sdcard";
    std::string output = "file1.txt\nfile2.txt\n";
    std::string script = "OKAY" "OKAY" + output;

    std::string written;
    auto transport = std::make_unique<RecordingTransport>(script, &written, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));

    std::string result;
    AdbError err = client.shellCommand(serial, command, &result);

    CHECK(err.ok);
    CHECK_STR_EQ(result, output);

    std::string expectedWritten =
        encodeHostRequest("host:transport:" + serial) + encodeHostRequest("shell:" + command);
    CHECK_STR_EQ(written, expectedWritten);
}

// ---------------------------------------------------------------------
// Guards that must run before any I/O
// ---------------------------------------------------------------------

TEST(AdbClientSuite, pathTooLongIsRejectedBeforeAnyIoForAllSyncMethods) {
    std::string longPath(SYNC_PATH_MAX + 1, 'x');

    {
        bool factoryCalled = false;
        TransportFactory factory = [&](std::string*) -> std::unique_ptr<Transport> {
            factoryCalled = true;
            return nullptr;
        };
        AdbClient client(factory);
        std::vector<DirEntry> entries;
        AdbError err = client.syncList("SERIAL1", longPath, &entries);
        CHECK(!err.ok);
        CHECK(err.message.find(longPath) != std::string::npos);
        CHECK(!factoryCalled);
    }
    {
        bool factoryCalled = false;
        TransportFactory factory = [&](std::string*) -> std::unique_ptr<Transport> {
            factoryCalled = true;
            return nullptr;
        };
        AdbClient client(factory);
        DirEntry entry;
        bool exists = false;
        AdbError err = client.syncStat("SERIAL1", longPath, &entry, &exists);
        CHECK(!err.ok);
        CHECK(err.message.find(longPath) != std::string::npos);
        CHECK(!factoryCalled);
    }
    {
        bool factoryCalled = false;
        TransportFactory factory = [&](std::string*) -> std::unique_ptr<Transport> {
            factoryCalled = true;
            return nullptr;
        };
        AdbClient client(factory);
        TempFile temp;
        ProgressFn progress = [](uint64_t, uint64_t) { return true; };
        AdbError err = client.syncRecv("SERIAL1", longPath, temp.fd(), 0, progress);
        CHECK(!err.ok);
        CHECK(err.message.find(longPath) != std::string::npos);
        CHECK(!factoryCalled);
    }
    {
        bool factoryCalled = false;
        TransportFactory factory = [&](std::string*) -> std::unique_ptr<Transport> {
            factoryCalled = true;
            return nullptr;
        };
        AdbClient client(factory);
        TempFile temp;
        ProgressFn progress = [](uint64_t, uint64_t) { return true; };
        AdbError err = client.syncSend("SERIAL1", temp.fd(), 0, longPath, 0644, 0, progress);
        CHECK(!err.ok);
        CHECK(err.message.find(longPath) != std::string::npos);
        CHECK(!factoryCalled);
    }
}

TEST(AdbClientSuite, nullptrFactoryResultSurfacesErrorWithoutCrashing) {
    TransportFactory factory = [](std::string* err) -> std::unique_ptr<Transport> {
        if (err != nullptr) {
            *err = "no transport available";
        }
        return nullptr;
    };
    AdbClient client(factory);

    std::vector<DeviceInfo> devices;
    AdbError err = client.listDevices(&devices);

    CHECK(!err.ok);
    CHECK_STR_EQ(err.message, "no transport available");
}
