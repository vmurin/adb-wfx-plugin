// The ADB client: drives the wire protocol (host requests, sync v1,
// shell:) over the Transport seam built in Task 5, using the pure codec in
// adbproto.hpp for every byte encoded or decoded. This is the only place
// that decides *what conversation* to have with the adb server; it never
// touches a socket directly (that's TcpTransport's job) and never touches
// the local filesystem beyond reading/writing an already-open fd (that's
// PluginCore's job, Task 8). See docs/plan-adb-wfx.md (Task 7) and
// constraints.md's protocol reference for the wire-format this was
// implemented against.
#ifndef ADB_WFX_ADBCLIENT_HPP
#define ADB_WFX_ADBCLIENT_HPP

#include "adbproto.hpp"
#include "transport.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

struct AdbError {
    bool ok = true;
    std::string message;
};

// Return false to cancel the transfer. Called at least once per chunk, with
// the number of bytes transferred so far and the total (0 when unknown).
using ProgressFn = std::function<bool(uint64_t done, uint64_t total)>;

// Sentinel error message used when the caller cancelled.
inline constexpr const char* ADB_CANCELLED = "cancelled";

namespace adbclient_detail {

// Bytes read at a time while draining shell: output to EOF.
constexpr size_t SHELL_READ_CHUNK_SIZE = 4096;

inline AdbError makeError(std::string message) {
    return AdbError{false, std::move(message)};
}

// Wraps a low-level Transport failure with a human-readable prefix and
// whatever detail the transport captured (e.g. strerror() text), so a
// connection failure never surfaces as an empty message.
inline AdbError connectionError(const Transport& t, const std::string& what) {
    std::string detail = t.lastError();
    return makeError(detail.empty() ? what : (what + ": " + detail));
}

// Reads a 4-hex-digit length prefix followed by that many bytes -- the
// framing used for a host-level FAIL message and for host:devices-l's
// response payload.
inline AdbError readLengthPrefixedMessage(Transport* t, std::string* out) {
    char lenBuf[4];
    if (!t->readExactly(lenBuf, sizeof(lenBuf))) {
        return connectionError(*t, "failed to read message length from adb server");
    }
    bool ok = false;
    uint32_t len = parseHexLength(lenBuf, &ok);
    if (!ok) {
        return makeError("malformed length prefix from adb server");
    }
    out->assign(len, '\0');
    if (len > 0 && !t->readExactly(&(*out)[0], len)) {
        return connectionError(*t, "failed to read message from adb server");
    }
    return AdbError{};
}

// Sends a framed host-style request (used for host:devices-l,
// host:transport:<serial>, sync:, and shell:<command> alike) and reads the
// OKAY/FAIL status that follows every one of them. On FAIL, reads and
// returns the server's message verbatim.
inline AdbError sendRequestAndCheckStatus(Transport* t, const std::string& service) {
    std::string request = encodeHostRequest(service);
    if (!t->writeAll(request.data(), request.size())) {
        return connectionError(*t, "failed to write request to adb server");
    }
    char statusBuf[4];
    if (!t->readExactly(statusBuf, sizeof(statusBuf))) {
        return connectionError(*t, "failed to read status from adb server");
    }
    AdbStatus status = parseStatus(statusBuf);
    if (status == AdbStatus::Okay) {
        return AdbError{};
    }
    if (status == AdbStatus::Fail) {
        std::string message;
        AdbError readErr = readLengthPrefixedMessage(t, &message);
        if (!readErr.ok) {
            return readErr;
        }
        return makeError(message);
    }
    return makeError("malformed response from adb server");
}

// Writes one full sync packet: the 8-byte header (id + arg) followed by
// its payload, if any.
inline bool writeSyncPacket(Transport* t, const char* id, uint32_t arg, const void* payload,
                             size_t payloadLen) {
    std::array<unsigned char, 8> header = encodeSyncHeader(id, arg);
    if (!t->writeAll(header.data(), header.size())) {
        return false;
    }
    if (payloadLen > 0 && !t->writeAll(payload, payloadLen)) {
        return false;
    }
    return true;
}

inline bool readSyncHeader(Transport* t, SyncHeader* out) {
    unsigned char buf[8];
    if (!t->readExactly(buf, sizeof(buf))) {
        return false;
    }
    *out = parseSyncHeader(buf);
    return true;
}

// Reads a sync-protocol FAIL body (arg is the message length, already
// consumed from the 8-byte header by the caller) and turns it into an
// AdbError.
inline AdbError readSyncFailMessage(Transport* t, uint32_t len) {
    std::string message;
    message.assign(len, '\0');
    if (len > 0 && !t->readExactly(&message[0], len)) {
        return connectionError(*t, "failed to read FAIL message from adb server");
    }
    return makeError(message);
}

// Reads one LIST-reply entry. DENT and the terminal DONE share a shape --
// id + mode + size + mtime + namelen + name bytes (DONE's fields are all
// zero and namelen is 0) -- which is not the generic 8-byte header used by
// every other sync packet type (see constraints.md: "DENT is the one
// packet where the second field is not a length"). FAIL still uses the
// generic 8-byte-header + message shape.
inline AdbError readListEntry(Transport* t, DirEntry* outEntry, bool* isDent, bool* done) {
    *isDent = false;
    *done = false;

    char idBuf[4];
    if (!t->readExactly(idBuf, sizeof(idBuf))) {
        return connectionError(*t, "failed to read LIST reply from adb server");
    }

    if (std::memcmp(idBuf, "FAIL", 4) == 0) {
        unsigned char lenBuf[4];
        if (!t->readExactly(lenBuf, sizeof(lenBuf))) {
            return connectionError(*t, "failed to read FAIL length from adb server");
        }
        return readSyncFailMessage(t, readU32Le(lenBuf));
    }

    bool isDentId = std::memcmp(idBuf, "DENT", 4) == 0;
    bool isDoneId = std::memcmp(idBuf, "DONE", 4) == 0;
    if (!isDentId && !isDoneId) {
        return makeError("malformed LIST reply from adb server");
    }

    constexpr size_t kFixedFieldsSize = 16; // mode + size + mtime + namelen
    unsigned char fixed[kFixedFieldsSize];
    if (!t->readExactly(fixed, sizeof(fixed))) {
        return connectionError(*t, "failed to read LIST entry from adb server");
    }
    uint32_t namelen = readU32Le(fixed + 12);

    std::vector<unsigned char> block(kFixedFieldsSize + namelen);
    std::memcpy(block.data(), fixed, kFixedFieldsSize);
    if (namelen > 0 && !t->readExactly(block.data() + kFixedFieldsSize, namelen)) {
        return connectionError(*t, "failed to read LIST entry name from adb server");
    }

    if (isDoneId) {
        *done = true;
        return AdbError{};
    }

    size_t consumed = 0;
    if (!parseDentBody(block.data(), block.size(), outEntry, &consumed)) {
        return makeError("malformed DENT entry from adb server");
    }
    *isDent = true;
    return AdbError{};
}

inline bool shouldContinue(const ProgressFn& progress, uint64_t done, uint64_t total) {
    return !progress || progress(done, total);
}

// ::write() in a loop, handling short writes and EINTR -- the counterpart
// to Transport::writeAll, but for an already-open local fd instead of a
// socket.
inline bool writeAllToFd(int fd, const unsigned char* buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t result = ::write(fd, buf + done, n - done);
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

// ::read() in a loop, handling short reads and EINTR. Returns false (short
// local file) if fewer than n bytes are available before EOF.
inline bool readAllFromFd(int fd, unsigned char* buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t result = ::read(fd, buf + done, n - done);
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

} // namespace adbclient_detail

class AdbClient {
public:
    explicit AdbClient(TransportFactory factory) : factory_(std::move(factory)) {}

    AdbError listDevices(std::vector<DeviceInfo>* out) {
        out->clear();
        std::string factoryError;
        std::unique_ptr<Transport> t = factory_(&factoryError);
        if (!t) {
            return adbclient_detail::makeError(
                factoryError.empty() ? "failed to create transport" : factoryError);
        }

        AdbError statusErr = adbclient_detail::sendRequestAndCheckStatus(t.get(), "host:devices-l");
        if (!statusErr.ok) {
            t->close();
            return statusErr;
        }

        std::string payload;
        AdbError payloadErr = adbclient_detail::readLengthPrefixedMessage(t.get(), &payload);
        t->close();
        if (!payloadErr.ok) {
            return payloadErr;
        }
        *out = parseDevicesL(payload);
        return AdbError{};
    }

    AdbError syncList(const std::string& serial, const std::string& path,
                      std::vector<DirEntry>* out) {
        out->clear();
        if (AdbError guardErr; !checkPathLength(path, &guardErr)) {
            return guardErr;
        }

        AdbError err;
        std::unique_ptr<Transport> t = openSyncTransport(serial, &err);
        if (!t) {
            return err;
        }

        if (!adbclient_detail::writeSyncPacket(t.get(), "LIST", static_cast<uint32_t>(path.size()),
                                                path.data(), path.size())) {
            AdbError writeErr = adbclient_detail::connectionError(*t, "failed to write LIST request");
            t->close();
            return writeErr;
        }

        for (;;) {
            DirEntry entry;
            bool isDent = false;
            bool done = false;
            AdbError entryErr = adbclient_detail::readListEntry(t.get(), &entry, &isDent, &done);
            if (!entryErr.ok) {
                out->clear();
                t->close();
                return entryErr;
            }
            if (done) {
                break;
            }
            if (isDent) {
                out->push_back(std::move(entry));
            }
        }

        t->close();
        return AdbError{};
    }

    AdbError syncStat(const std::string& serial, const std::string& path, DirEntry* out,
                      bool* exists) {
        *out = DirEntry{};
        *exists = false;
        if (AdbError guardErr; !checkPathLength(path, &guardErr)) {
            return guardErr;
        }

        AdbError err;
        std::unique_ptr<Transport> t = openSyncTransport(serial, &err);
        if (!t) {
            return err;
        }

        if (!adbclient_detail::writeSyncPacket(t.get(), "STAT", static_cast<uint32_t>(path.size()),
                                                path.data(), path.size())) {
            AdbError writeErr = adbclient_detail::connectionError(*t, "failed to write STAT request");
            t->close();
            return writeErr;
        }

        char idBuf[4];
        if (!t->readExactly(idBuf, sizeof(idBuf))) {
            AdbError readErr = adbclient_detail::connectionError(*t, "failed to read STAT reply from adb server");
            t->close();
            return readErr;
        }

        if (std::memcmp(idBuf, "FAIL", 4) == 0) {
            unsigned char lenBuf[4];
            AdbError result;
            if (!t->readExactly(lenBuf, sizeof(lenBuf))) {
                result = adbclient_detail::connectionError(*t, "failed to read FAIL length from adb server");
            } else {
                result = adbclient_detail::readSyncFailMessage(t.get(), readU32Le(lenBuf));
            }
            t->close();
            return result;
        }
        if (std::memcmp(idBuf, "STAT", 4) != 0) {
            t->close();
            return adbclient_detail::makeError("malformed STAT reply from adb server");
        }

        unsigned char body[12]; // mode + size + mtime
        if (!t->readExactly(body, sizeof(body))) {
            AdbError readErr = adbclient_detail::connectionError(*t, "failed to read STAT body from adb server");
            t->close();
            return readErr;
        }
        t->close();

        uint32_t mode = readU32Le(body);
        uint32_t size = readU32Le(body + 4);
        uint32_t mtime = readU32Le(body + 8);
        if (mode == 0 && size == 0 && mtime == 0) {
            return AdbError{}; // all-zero: does not exist
        }
        out->mode = mode;
        out->size = size;
        out->mtime = static_cast<int64_t>(mtime);
        *exists = true;
        return AdbError{};
    }

    // Writes the remote file into localFd (already open for writing).
    AdbError syncRecv(const std::string& serial, const std::string& remote, int localFd,
                      uint64_t expectedSize, const ProgressFn& progress) {
        if (AdbError guardErr; !checkPathLength(remote, &guardErr)) {
            return guardErr;
        }

        AdbError err;
        std::unique_ptr<Transport> t = openSyncTransport(serial, &err);
        if (!t) {
            return err;
        }

        if (!adbclient_detail::writeSyncPacket(t.get(), "RECV", static_cast<uint32_t>(remote.size()),
                                                remote.data(), remote.size())) {
            AdbError writeErr = adbclient_detail::connectionError(*t, "failed to write RECV request");
            t->close();
            return writeErr;
        }

        std::vector<unsigned char> buffer(SYNC_DATA_MAX);
        uint64_t done = 0;
        for (;;) {
            SyncHeader header;
            if (!adbclient_detail::readSyncHeader(t.get(), &header)) {
                AdbError readErr = adbclient_detail::connectionError(*t, "failed to read sync reply from adb server");
                t->close();
                return readErr;
            }
            if (syncIdIs(header, "DONE")) {
                break;
            }
            if (syncIdIs(header, "FAIL")) {
                AdbError result = adbclient_detail::readSyncFailMessage(t.get(), header.arg);
                t->close();
                return result;
            }
            if (!syncIdIs(header, "DATA")) {
                t->close();
                return adbclient_detail::makeError("malformed sync reply from adb server");
            }
            if (header.arg > SYNC_DATA_MAX) {
                t->close();
                return adbclient_detail::makeError("adb server sent an oversized DATA chunk");
            }

            if (!t->readExactly(buffer.data(), header.arg)) {
                AdbError readErr = adbclient_detail::connectionError(*t, "failed to read DATA chunk from adb server");
                t->close();
                return readErr;
            }
            if (!adbclient_detail::writeAllToFd(localFd, buffer.data(), header.arg)) {
                t->close();
                return adbclient_detail::makeError("failed to write to local file");
            }

            done += header.arg;
            if (!adbclient_detail::shouldContinue(progress, done, expectedSize)) {
                t->close();
                return AdbError{false, ADB_CANCELLED};
            }
        }

        t->close();
        return AdbError{};
    }

    AdbError syncSend(const std::string& serial, int localFd, uint64_t localSize,
                      const std::string& remote, uint32_t mode, int64_t mtime,
                      const ProgressFn& progress) {
        if (AdbError guardErr; !checkPathLength(remote, &guardErr)) {
            return guardErr;
        }

        AdbError err;
        std::unique_ptr<Transport> t = openSyncTransport(serial, &err);
        if (!t) {
            return err;
        }

        std::string spec = encodeSendPathSpec(remote, mode);
        if (!adbclient_detail::writeSyncPacket(t.get(), "SEND", static_cast<uint32_t>(spec.size()),
                                                spec.data(), spec.size())) {
            AdbError writeErr = adbclient_detail::connectionError(*t, "failed to write SEND request");
            t->close();
            return writeErr;
        }

        std::vector<unsigned char> buffer(SYNC_DATA_MAX);
        uint64_t done = 0;
        while (done < localSize) {
            uint64_t remaining = localSize - done;
            uint32_t chunkLen = static_cast<uint32_t>(std::min<uint64_t>(remaining, SYNC_DATA_MAX));

            if (!adbclient_detail::readAllFromFd(localFd, buffer.data(), chunkLen)) {
                t->close();
                return adbclient_detail::makeError("failed to read from local file");
            }
            if (!adbclient_detail::writeSyncPacket(t.get(), "DATA", chunkLen, buffer.data(), chunkLen)) {
                AdbError writeErr = adbclient_detail::connectionError(*t, "failed to write DATA chunk");
                t->close();
                return writeErr;
            }

            done += chunkLen;
            if (!adbclient_detail::shouldContinue(progress, done, localSize)) {
                t->close();
                return AdbError{false, ADB_CANCELLED};
            }
        }

        uint32_t mtimeArg = static_cast<uint32_t>(static_cast<uint64_t>(mtime));
        if (!adbclient_detail::writeSyncPacket(t.get(), "DONE", mtimeArg, nullptr, 0)) {
            AdbError writeErr = adbclient_detail::connectionError(*t, "failed to write DONE");
            t->close();
            return writeErr;
        }

        SyncHeader finalHeader;
        if (!adbclient_detail::readSyncHeader(t.get(), &finalHeader)) {
            AdbError readErr = adbclient_detail::connectionError(*t, "failed to read final reply from adb server");
            t->close();
            return readErr;
        }
        if (syncIdIs(finalHeader, "FAIL")) {
            AdbError result = adbclient_detail::readSyncFailMessage(t.get(), finalHeader.arg);
            t->close();
            return result;
        }
        if (!syncIdIs(finalHeader, "OKAY")) {
            t->close();
            return adbclient_detail::makeError("malformed final reply from adb server");
        }

        t->close();
        return AdbError{};
    }

    // Combined stdout+stderr of the command.
    AdbError shellCommand(const std::string& serial, const std::string& command,
                          std::string* output) {
        output->clear();

        AdbError err;
        std::unique_ptr<Transport> t = openDeviceTransport(serial, &err);
        if (!t) {
            return err;
        }

        AdbError shellErr = adbclient_detail::sendRequestAndCheckStatus(t.get(), "shell:" + command);
        if (!shellErr.ok) {
            t->close();
            return shellErr;
        }

        unsigned char buffer[adbclient_detail::SHELL_READ_CHUNK_SIZE];
        for (;;) {
            ptrdiff_t n = t->readSome(buffer, sizeof(buffer));
            if (n < 0) {
                AdbError readErr = adbclient_detail::connectionError(*t, "failed to read shell output from adb server");
                t->close();
                return readErr;
            }
            if (n == 0) {
                break; // EOF: normal termination, not an error.
            }
            output->append(reinterpret_cast<char*>(buffer), static_cast<size_t>(n));
        }

        t->close();
        return AdbError{};
    }

private:
    // Rejects a remote path that is too long for the sync protocol, before
    // any transport is created and any byte is written.
    static bool checkPathLength(const std::string& path, AdbError* err) {
        if (path.size() > SYNC_PATH_MAX) {
            *err = adbclient_detail::makeError("remote path too long for sync protocol: " + path);
            return false;
        }
        return true;
    }

    // Opens a fresh transport and binds it to the given device via
    // host:transport:<serial>. The caller sends the next (device-scoped)
    // request on the same socket.
    std::unique_ptr<Transport> openDeviceTransport(const std::string& serial, AdbError* err) {
        std::string factoryError;
        std::unique_ptr<Transport> t = factory_(&factoryError);
        if (!t) {
            *err = adbclient_detail::makeError(
                factoryError.empty() ? "failed to create transport" : factoryError);
            return nullptr;
        }

        AdbError transportErr =
            adbclient_detail::sendRequestAndCheckStatus(t.get(), "host:transport:" + serial);
        if (!transportErr.ok) {
            t->close();
            *err = transportErr;
            return nullptr;
        }
        *err = AdbError{};
        return t;
    }

    // Opens a device transport and switches it into the binary sync
    // protocol via "sync:".
    std::unique_ptr<Transport> openSyncTransport(const std::string& serial, AdbError* err) {
        std::unique_ptr<Transport> t = openDeviceTransport(serial, err);
        if (!t) {
            return nullptr;
        }
        AdbError syncErr = adbclient_detail::sendRequestAndCheckStatus(t.get(), "sync:");
        if (!syncErr.ok) {
            t->close();
            *err = syncErr;
            return nullptr;
        }
        *err = AdbError{};
        return t;
    }

    TransportFactory factory_;
};

#endif // ADB_WFX_ADBCLIENT_HPP
