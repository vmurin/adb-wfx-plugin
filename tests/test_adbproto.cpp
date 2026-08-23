// Tests for adbproto.hpp: the pure ADB wire-protocol codec (host request
// framing, sync-v1 packet headers/bodies, and the `host:devices-l` text
// parser). No I/O anywhere -- every test builds its input bytes by hand.
// Written before the new adbproto.hpp functions exist (TDD).
#include "adbproto.hpp"
#include "testing.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------
// encodeHostRequest
// ---------------------------------------------------------------------

TEST(EncodeHostRequestSuite, devicesL) {
    CHECK_STR_EQ(encodeHostRequest("host:devices-l"), "000ehost:devices-l");
}

TEST(EncodeHostRequestSuite, version) {
    CHECK_STR_EQ(encodeHostRequest("host:version"), "000chost:version");
}

TEST(EncodeHostRequestSuite, empty) {
    CHECK_STR_EQ(encodeHostRequest(""), "0000");
}

TEST(EncodeHostRequestSuite, threeHundredByteService) {
    std::string service(300, 'x');
    std::string encoded = encodeHostRequest(service);
    CHECK_STR_EQ(encoded.substr(0, 4), "012c");
    CHECK_EQ(encoded.size(), static_cast<size_t>(4 + 300));
}

// ---------------------------------------------------------------------
// parseStatus
// ---------------------------------------------------------------------

TEST(ParseStatusSuite, okay) {
    CHECK(parseStatus("OKAY") == AdbStatus::Okay);
}

TEST(ParseStatusSuite, fail) {
    CHECK(parseStatus("FAIL") == AdbStatus::Fail);
}

TEST(ParseStatusSuite, malformed) {
    CHECK(parseStatus("XXXX") == AdbStatus::Malformed);
}

// ---------------------------------------------------------------------
// parseHexLength
// ---------------------------------------------------------------------

TEST(ParseHexLengthSuite, basic) {
    bool ok = false;
    CHECK_EQ(parseHexLength("000e", &ok), 14u);
    CHECK(ok);
}

TEST(ParseHexLengthSuite, maxFourDigit) {
    bool ok = false;
    CHECK_EQ(parseHexLength("ffff", &ok), 65535u);
    CHECK(ok);
}

TEST(ParseHexLengthSuite, uppercaseAccepted) {
    bool ok = false;
    CHECK_EQ(parseHexLength("FFFF", &ok), 65535u);
    CHECK(ok);
}

TEST(ParseHexLengthSuite, invalidDigit) {
    bool ok = true;
    parseHexLength("00G0", &ok);
    CHECK(!ok);
}

// ---------------------------------------------------------------------
// readU32Le / writeU32Le
// ---------------------------------------------------------------------

TEST(U32LeSuite, roundTripZero) {
    unsigned char buf[4];
    writeU32Le(buf, 0);
    CHECK_EQ(readU32Le(buf), 0u);
}

TEST(U32LeSuite, roundTripOne) {
    unsigned char buf[4];
    writeU32Le(buf, 1);
    CHECK_EQ(readU32Le(buf), 1u);
}

TEST(U32LeSuite, roundTripArbitrary) {
    unsigned char buf[4];
    writeU32Le(buf, 0x12345678u);
    CHECK_EQ(readU32Le(buf), 0x12345678u);
}

TEST(U32LeSuite, roundTripMax) {
    unsigned char buf[4];
    writeU32Le(buf, 0xFFFFFFFFu);
    CHECK_EQ(readU32Le(buf), 0xFFFFFFFFu);
}

TEST(U32LeSuite, explicitByteOrder) {
    unsigned char buf[4];
    writeU32Le(buf, 0x12345678u);
    CHECK_EQ(buf[0], 0x78);
    CHECK_EQ(buf[1], 0x56);
    CHECK_EQ(buf[2], 0x34);
    CHECK_EQ(buf[3], 0x12);
}

// ---------------------------------------------------------------------
// encodeSyncHeader / parseSyncHeader / syncIdIs
// ---------------------------------------------------------------------

TEST(SyncHeaderSuite, encode) {
    std::array<unsigned char, 8> header = encodeSyncHeader("SEND", 42);
    std::array<unsigned char, 8> expected = {'S', 'E', 'N', 'D', 42, 0, 0, 0};
    CHECK(header == expected);
}

TEST(SyncHeaderSuite, parse) {
    std::array<unsigned char, 8> encoded = encodeSyncHeader("SEND", 42);
    SyncHeader h = parseSyncHeader(encoded.data());
    CHECK(std::memcmp(h.id, "SEND", 4) == 0);
    CHECK_EQ(h.arg, 42u);
}

TEST(SyncHeaderSuite, syncIdIsMatches) {
    std::array<unsigned char, 8> encoded = encodeSyncHeader("SEND", 42);
    SyncHeader h = parseSyncHeader(encoded.data());
    CHECK(syncIdIs(h, "SEND"));
    CHECK(!syncIdIs(h, "RECV"));
}

// ---------------------------------------------------------------------
// parseDentBody
// ---------------------------------------------------------------------

namespace {

// Builds a DENT body (everything after the 4 id bytes): mode, size,
// mtime, namelen (each u32 LE), followed by the raw name bytes.
std::vector<unsigned char> buildDentBody(uint32_t mode, uint32_t size,
                                          uint32_t mtime,
                                          const std::string& name) {
    std::vector<unsigned char> body(16);
    writeU32Le(body.data(), mode);
    writeU32Le(body.data() + 4, size);
    writeU32Le(body.data() + 8, mtime);
    writeU32Le(body.data() + 12, static_cast<uint32_t>(name.size()));
    body.insert(body.end(), name.begin(), name.end());
    return body;
}

} // namespace

TEST(ParseDentBodySuite, regularFile) {
    std::vector<unsigned char> body = buildDentBody(33188, 1234, 1700000000, "photo.jpg");
    DirEntry out;
    size_t consumed = 0;
    bool ok = parseDentBody(body.data(), body.size(), &out, &consumed);
    CHECK(ok);
    CHECK_STR_EQ(out.name, "photo.jpg");
    CHECK_EQ(out.mode, 33188u);
    CHECK_EQ(out.size, 1234u);
    CHECK_EQ(out.mtime, static_cast<int64_t>(1700000000));
    CHECK(!out.isDir());
    CHECK(!out.isSymlink());
    CHECK_EQ(consumed, static_cast<size_t>(16 + 9));
}

TEST(ParseDentBodySuite, directory) {
    std::vector<unsigned char> body = buildDentBody(16877, 0, 1700000000, "DCIM");
    DirEntry out;
    size_t consumed = 0;
    bool ok = parseDentBody(body.data(), body.size(), &out, &consumed);
    CHECK(ok);
    CHECK(out.isDir());
    CHECK(!out.isSymlink());
}

TEST(ParseDentBodySuite, symlink) {
    std::vector<unsigned char> body = buildDentBody(41471, 0, 1700000000, "link");
    DirEntry out;
    size_t consumed = 0;
    bool ok = parseDentBody(body.data(), body.size(), &out, &consumed);
    CHECK(ok);
    CHECK(out.isSymlink());
    CHECK(!out.isDir());
}

TEST(ParseDentBodySuite, utf8NameWithCyrillicAndSpace) {
    std::string name = "фото кошка.jpg"; // "фото кошка.jpg"
    std::vector<unsigned char> body = buildDentBody(33188, 42, 1700000000, name);
    DirEntry out;
    size_t consumed = 0;
    bool ok = parseDentBody(body.data(), body.size(), &out, &consumed);
    CHECK(ok);
    CHECK_STR_EQ(out.name, name);
    CHECK_EQ(consumed, static_cast<size_t>(16 + name.size()));
}

TEST(ParseDentBodySuite, availOneByteShortOfName) {
    std::vector<unsigned char> body = buildDentBody(33188, 1234, 1700000000, "photo.jpg");
    body.pop_back(); // avail is now one byte short of the full name
    DirEntry out;
    size_t consumed = 0;
    bool ok = parseDentBody(body.data(), body.size(), &out, &consumed);
    CHECK(!ok);
}

TEST(ParseDentBodySuite, hugeNamelenReturnsFalseWithoutOverflow) {
    std::vector<unsigned char> body(16);
    writeU32Le(body.data(), 33188);
    writeU32Le(body.data() + 4, 0);
    writeU32Le(body.data() + 8, 0);
    writeU32Le(body.data() + 12, 0xFFFFFFFFu);
    DirEntry out;
    size_t consumed = 0;
    bool ok = parseDentBody(body.data(), body.size(), &out, &consumed);
    CHECK(!ok);
}

TEST(ParseDentBodySuite, availShorterThanFixedHeaderReturnsFalse) {
    std::vector<unsigned char> body(15, 0); // one byte short of the 16-byte fixed header
    DirEntry out;
    size_t consumed = 0;
    bool ok = parseDentBody(body.data(), body.size(), &out, &consumed);
    CHECK(!ok);
}

// ---------------------------------------------------------------------
// encodeSendPathSpec
// ---------------------------------------------------------------------

TEST(EncodeSendPathSpecSuite, basic) {
    CHECK_STR_EQ(encodeSendPathSpec("/sdcard/a b.jpg", 33188), "/sdcard/a b.jpg,33188");
}

// ---------------------------------------------------------------------
// parseDevicesL
// ---------------------------------------------------------------------

TEST(ParseDevicesLSuite, fixtureWithMixedForms) {
    std::string text =
        "List of devices attached\n"
        "27281FDH2008DM         device usb:34603008X product:panther model:Pixel_7 device:panther transport_id:28\n"
        "emulator-5554          offline\n"
        "BADCAFE                unauthorized\n";

    std::vector<DeviceInfo> devices = parseDevicesL(text);
    CHECK_EQ(devices.size(), static_cast<size_t>(3));

    CHECK_STR_EQ(devices[0].serial, "27281FDH2008DM");
    CHECK_STR_EQ(devices[0].state, "device");
    CHECK_STR_EQ(devices[0].model, "Pixel_7");

    CHECK_STR_EQ(devices[1].serial, "emulator-5554");
    CHECK_STR_EQ(devices[1].state, "offline");
    CHECK_STR_EQ(devices[1].model, "");

    CHECK_STR_EQ(devices[2].serial, "BADCAFE");
    CHECK_STR_EQ(devices[2].state, "unauthorized");
    CHECK_STR_EQ(devices[2].model, "");

    CHECK(deviceStateIsUsable(devices[0].state));
    CHECK(!deviceStateIsUsable(devices[1].state));
    CHECK(!deviceStateIsUsable(devices[2].state));
}

TEST(ParseDevicesLSuite, tabSeparatedShortForm) {
    std::string text = "List of devices attached\nABC123\tdevice\n";
    std::vector<DeviceInfo> devices = parseDevicesL(text);
    CHECK_EQ(devices.size(), static_cast<size_t>(1));
    CHECK_STR_EQ(devices[0].serial, "ABC123");
    CHECK_STR_EQ(devices[0].state, "device");
    CHECK_STR_EQ(devices[0].model, "");
}

TEST(ParseDevicesLSuite, crlfAndTrailingWhitespaceTolerated) {
    std::string text = "List of devices attached\r\nABC123\tdevice  \r\n\r\n";
    std::vector<DeviceInfo> devices = parseDevicesL(text);
    CHECK_EQ(devices.size(), static_cast<size_t>(1));
    CHECK_STR_EQ(devices[0].serial, "ABC123");
    CHECK_STR_EQ(devices[0].state, "device");
}

TEST(ParseDevicesLSuite, headerAndBlankLinesOnly) {
    std::vector<DeviceInfo> devices = parseDevicesL("List of devices attached\n\n");
    CHECK_EQ(devices.size(), static_cast<size_t>(0));
}

TEST(ParseDevicesLSuite, emptyString) {
    std::vector<DeviceInfo> devices = parseDevicesL("");
    CHECK_EQ(devices.size(), static_cast<size_t>(0));
}

// ---------------------------------------------------------------------
// deviceStateIsUsable / deviceStateMessage
// ---------------------------------------------------------------------

TEST(DeviceStateSuite, onlyDeviceIsUsable) {
    CHECK(deviceStateIsUsable("device"));
    CHECK(!deviceStateIsUsable("unauthorized"));
    CHECK(!deviceStateIsUsable("offline"));
    CHECK(!deviceStateIsUsable("recovery"));
    CHECK(!deviceStateIsUsable("bootloader"));
    CHECK(!deviceStateIsUsable("sideload"));
}

TEST(DeviceStateSuite, unauthorizedMessageMentionsAuthorizingAndSerial) {
    DeviceInfo d;
    d.serial = "27281FDH2008DM";
    d.state = "unauthorized";
    std::string msg = deviceStateMessage(d);
    CHECK(!msg.empty());
    CHECK(msg.find(d.serial) != std::string::npos);
    bool mentionsAuthorising =
        msg.find("author") != std::string::npos || msg.find("Author") != std::string::npos;
    bool mentionsScreen = msg.find("screen") != std::string::npos;
    CHECK(mentionsAuthorising);
    CHECK(mentionsScreen);
}

TEST(DeviceStateSuite, offlineMessageMentionsReconnectingAndSerial) {
    DeviceInfo d;
    d.serial = "emulator-5554";
    d.state = "offline";
    std::string msg = deviceStateMessage(d);
    CHECK(!msg.empty());
    CHECK(msg.find(d.serial) != std::string::npos);
    bool mentionsReconnect =
        msg.find("reconnect") != std::string::npos || msg.find("Reconnect") != std::string::npos;
    CHECK(mentionsReconnect);
}
