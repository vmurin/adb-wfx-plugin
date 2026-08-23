// One entry from the ADB sync protocol's LIST response, plus the pure ADB
// wire-protocol codec built on top of it: host-request framing, the
// OKAY/FAIL status word, sync-v1 packet headers/bodies, the SEND path
// spec, and the `host:devices-l` text format. Everything here is bytes
// in, values out -- no sockets, no files, no processes. See
// docs/plan-adb-wfx.md (Task 4) for the wire-format reference this was
// implemented against.
#ifndef ADB_WFX_ADBPROTO_HPP
#define ADB_WFX_ADBPROTO_HPP

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

constexpr uint32_t POSIX_MODE_TYPE_MASK = 0170000;
constexpr uint32_t POSIX_MODE_DIRECTORY = 0040000;
constexpr uint32_t POSIX_MODE_SYMLINK = 0120000;

struct DirEntry {
    std::string name;
    uint32_t mode = 0;
    uint64_t size = 0;
    int64_t mtime = 0;

    bool isDir() const {
        return (mode & POSIX_MODE_TYPE_MASK) == POSIX_MODE_DIRECTORY;
    }

    bool isSymlink() const {
        return (mode & POSIX_MODE_TYPE_MASK) == POSIX_MODE_SYMLINK;
    }
};

// Max payload per sync DATA chunk, and the cap on sync path strings.
constexpr uint32_t SYNC_DATA_MAX = 65536;
constexpr uint32_t SYNC_PATH_MAX = 1024;
constexpr int ADB_DEFAULT_PORT = 5037;

// Frames a host-service request: 4 lowercase hex digits of the payload
// length, followed by the payload itself. E.g. "host:version" (12 bytes)
// becomes "000chost:version".
inline std::string encodeHostRequest(const std::string& service) {
    char lengthPrefix[5];
    std::snprintf(lengthPrefix, sizeof(lengthPrefix), "%04x",
                  static_cast<unsigned int>(service.size()));
    return std::string(lengthPrefix, 4) + service;
}

enum class AdbStatus { Okay, Fail, Malformed };

// Interprets the 4-byte ASCII status word that follows every ADB request.
inline AdbStatus parseStatus(const char* fourBytes) {
    if (std::memcmp(fourBytes, "OKAY", 4) == 0) {
        return AdbStatus::Okay;
    }
    if (std::memcmp(fourBytes, "FAIL", 4) == 0) {
        return AdbStatus::Fail;
    }
    return AdbStatus::Malformed;
}

// Little-endian u32 helpers used throughout the sync protocol.
inline uint32_t readU32Le(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline void writeU32Le(unsigned char* p, uint32_t v) {
    p[0] = static_cast<unsigned char>(v & 0xFF);
    p[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
    p[2] = static_cast<unsigned char>((v >> 16) & 0xFF);
    p[3] = static_cast<unsigned char>((v >> 24) & 0xFF);
}

namespace detail {

inline int hexDigitValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

} // namespace detail

// Parses a 4-digit hex length prefix. Accepts upper- or lowercase digits;
// sets *ok = false (and returns 0) on any non-hex digit.
inline uint32_t parseHexLength(const char* fourHexDigits, bool* ok) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        int digit = detail::hexDigitValue(fourHexDigits[i]);
        if (digit < 0) {
            *ok = false;
            return 0;
        }
        value = (value << 4) | static_cast<uint32_t>(digit);
    }
    *ok = true;
    return value;
}

struct SyncHeader {
    char id[4];
    uint32_t arg;
};

// Encodes an 8-byte sync packet header: 4 ASCII id bytes + 4-byte
// little-endian arg (a length for every packet type except DENT, where
// it is the file mode).
inline std::array<unsigned char, 8> encodeSyncHeader(const char* id, uint32_t arg) {
    std::array<unsigned char, 8> header{};
    std::memcpy(header.data(), id, 4);
    writeU32Le(header.data() + 4, arg);
    return header;
}

inline SyncHeader parseSyncHeader(const unsigned char* eightBytes) {
    SyncHeader header{};
    std::memcpy(header.id, eightBytes, 4);
    header.arg = readU32Le(eightBytes + 4);
    return header;
}

inline bool syncIdIs(const SyncHeader& h, const char* id) {
    return std::memcmp(h.id, id, 4) == 0;
}

// Parses the body of a DENT packet (everything after the 4 id bytes):
// mode, size, mtime, namelen (each u32 LE), followed by namelen bytes of
// name. Returns false if `avail` is too small for the fixed 16-byte
// header or for the name that follows -- including a hostile namelen
// like 0xFFFFFFFF, which is rejected by comparing against the available
// byte count rather than by adding namelen to a pointer or offset.
inline bool parseDentBody(const unsigned char* p, size_t avail, DirEntry* out,
                           size_t* consumed) {
    constexpr size_t DENT_FIXED_HEADER_SIZE = 16; // mode + size + mtime + namelen
    if (avail < DENT_FIXED_HEADER_SIZE) {
        return false;
    }

    uint32_t mode = readU32Le(p);
    uint32_t size = readU32Le(p + 4);
    uint32_t mtime = readU32Le(p + 8);
    uint32_t namelen = readU32Le(p + 12);

    size_t remaining = avail - DENT_FIXED_HEADER_SIZE;
    if (namelen > remaining) {
        return false;
    }

    out->mode = mode;
    out->size = size;
    out->mtime = static_cast<int64_t>(mtime);
    out->name.assign(reinterpret_cast<const char*>(p + DENT_FIXED_HEADER_SIZE), namelen);
    *consumed = DENT_FIXED_HEADER_SIZE + namelen;
    return true;
}

// "<path>,<mode-decimal>" -- the payload of a SEND request's path spec.
inline std::string encodeSendPathSpec(const std::string& path, uint32_t mode) {
    return path + "," + std::to_string(mode);
}

struct DeviceInfo {
    std::string serial;
    std::string state;
    std::string model;
};

namespace detail {

inline std::string trimTrailingWhitespace(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' ||
                        s[end - 1] == '\n')) {
        --end;
    }
    return s.substr(0, end);
}

// Splits a whitespace-run-separated line into fields, treating any run of
// spaces/tabs as a single separator (so both the tab-separated short form
// and the space-padded `-l` form parse the same way).
inline std::vector<std::string> splitFields(const std::string& line) {
    std::vector<std::string> fields;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
            ++i;
        }
        if (i > start) {
            fields.push_back(line.substr(start, i - start));
        }
    }
    return fields;
}

} // namespace detail

// Parses the text response of `host:devices-l`: one device per line,
// tolerating the header line, blank lines, trailing whitespace and
// CRLF line endings, and both the tab-separated short form and the
// space-padded `-l` form (serial, state, then space-separated
// `key:value` fields -- `model:` becomes DeviceInfo::model).
inline std::vector<DeviceInfo> parseDevicesL(const std::string& text) {
    std::vector<DeviceInfo> devices;

    size_t lineStart = 0;
    while (lineStart <= text.size()) {
        size_t lineEnd = text.find('\n', lineStart);
        std::string rawLine = (lineEnd == std::string::npos)
                                   ? text.substr(lineStart)
                                   : text.substr(lineStart, lineEnd - lineStart);
        std::string line = detail::trimTrailingWhitespace(rawLine);

        if (!line.empty() && line != "List of devices attached") {
            std::vector<std::string> fields = detail::splitFields(line);
            if (fields.size() >= 2) {
                DeviceInfo device;
                device.serial = fields[0];
                device.state = fields[1];
                for (size_t i = 2; i < fields.size(); ++i) {
                    constexpr char MODEL_PREFIX[] = "model:";
                    constexpr size_t MODEL_PREFIX_LEN = sizeof(MODEL_PREFIX) - 1;
                    if (fields[i].compare(0, MODEL_PREFIX_LEN, MODEL_PREFIX) == 0) {
                        device.model = fields[i].substr(MODEL_PREFIX_LEN);
                    }
                }
                devices.push_back(std::move(device));
            }
        }

        if (lineEnd == std::string::npos) {
            break;
        }
        lineStart = lineEnd + 1;
    }

    return devices;
}

// Only the "device" state means the phone is actually usable.
inline bool deviceStateIsUsable(const std::string& state) {
    return state == "device";
}

// A human-readable sentence describing what to do about a non-usable
// device state. Always contains the serial so it identifies which phone.
inline std::string deviceStateMessage(const DeviceInfo& d) {
    if (d.state == "unauthorized") {
        return "Device " + d.serial +
               " is unauthorized: check the phone's screen and authorise this computer.";
    }
    if (d.state == "offline") {
        return "Device " + d.serial + " is offline: try reconnecting the USB cable.";
    }
    if (d.state == "recovery") {
        return "Device " + d.serial + " is in recovery mode, not the normal Android system.";
    }
    if (d.state == "bootloader") {
        return "Device " + d.serial + " is in bootloader mode, not the normal Android system.";
    }
    if (d.state == "sideload") {
        return "Device " + d.serial + " is in sideload mode, not the normal Android system.";
    }
    return "Device " + d.serial + " is in state '" + d.state + "' and is not usable.";
}

#endif // ADB_WFX_ADBPROTO_HPP
