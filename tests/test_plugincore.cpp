// Tests for fsplugin_impl.hpp: PluginCore, the WFX logic layer. Written
// before fsplugin_impl.hpp exists (TDD) -- see
// .superpowers/sdd/plan-adb-wfx/task-8-report.md for the RED run. Every
// test here is driven by scripted FakeTransports through a real AdbClient;
// nothing touches a real adb server or phone. Local temp files live in a
// unique per-test directory, cleaned up on destruction.
#include "fsplugin_impl.hpp"

#include "adbclient.hpp"
#include "fake_transport.hpp"
#include "testing.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

// ---------------------------------------------------------------------
// Test fixtures shared across this file
// ---------------------------------------------------------------------

// A FakeTransport that, on destruction, copies its written() bytes and
// closed state into caller-owned storage -- AdbClient owns the Transport
// it gets from the factory in a local unique_ptr and destroys it before
// control returns to the caller, so this is the only way to inspect what
// was written once a PluginCore call has returned. Mirrors
// tests/test_adbclient.cpp's RecordingTransport.
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

// Wraps a single transport in a TransportFactory that hands it out exactly
// once. A second call fails loudly instead of silently returning nullptr.
// Mirrors tests/test_adbclient.cpp's singleUseFactory.
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

// Hands transports out of a queue, one per factory call, in the order
// pushed -- for PluginCore operations that make more than one AdbClient
// call (e.g. getFile's syncStat then syncRecv, or a listDirectory
// followed by a mutating call). callCount() lets a test assert exactly
// how many transports (i.e. network round trips) an operation used --
// in particular, that a second identical listDirectory call made zero.
class QueueFactory {
public:
    void push(std::unique_ptr<Transport> t) {
        queue_.push_back(std::move(t));
    }

    int callCount() const {
        return callCount_;
    }

    TransportFactory asFactory() {
        return [this](std::string* err) -> std::unique_ptr<Transport> {
            ++callCount_;
            if (queue_.empty()) {
                if (err != nullptr) {
                    *err = "test factory: queue exhausted";
                }
                return nullptr;
            }
            std::unique_ptr<Transport> t = std::move(queue_.front());
            queue_.pop_front();
            return t;
        };
    }

private:
    std::deque<std::unique_ptr<Transport>> queue_;
    int callCount_ = 0;
};

// A unique temp directory, removed (along with anything a test put in it)
// on destruction. Per the task's rule: local temp files are fine and
// necessary, but every test gets its own directory.
class TestTempDir {
public:
    TestTempDir() {
        const char* env = std::getenv("TMPDIR");
        std::string base = (env != nullptr && *env != '\0') ? env : "/tmp";
        if (base.back() != '/') {
            base += '/';
        }
        std::string tmpl = base + "adbwfx_plugincore_XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        char* result = ::mkdtemp(buf.data());
        CHECK(result != nullptr);
        path_ = result;
    }

    ~TestTempDir() {
        DIR* dir = ::opendir(path_.c_str());
        if (dir != nullptr) {
            struct dirent* entry;
            while ((entry = ::readdir(dir)) != nullptr) {
                std::string name = entry->d_name;
                if (name != "." && name != "..") {
                    ::unlink((path_ + "/" + name).c_str());
                }
            }
            ::closedir(dir);
        }
        ::rmdir(path_.c_str());
    }

    const std::string& path() const {
        return path_;
    }

    // Number of directory entries other than "." and "..". Used to prove
    // no temp/leftover file was created (or that exactly the expected
    // final file exists).
    size_t entryCount() const {
        size_t count = 0;
        DIR* dir = ::opendir(path_.c_str());
        if (dir != nullptr) {
            struct dirent* entry;
            while ((entry = ::readdir(dir)) != nullptr) {
                std::string name = entry->d_name;
                if (name != "." && name != "..") {
                    ++count;
                }
            }
            ::closedir(dir);
        }
        return count;
    }

private:
    std::string path_;
};

void writeFile(const std::string& path, const std::string& content) {
    FILE* fp = std::fopen(path.c_str(), "wb");
    CHECK(fp != nullptr);
    if (fp != nullptr) {
        size_t written = std::fwrite(content.data(), 1, content.size(), fp);
        CHECK_EQ(written, content.size());
        std::fclose(fp);
    }
}

std::string readFile(const std::string& path) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) {
        return std::string();
    }
    std::string result;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
        result.append(buf, n);
    }
    std::fclose(fp);
    return result;
}

// ---------------------------------------------------------------------
// Wire-format helpers built on adbproto.hpp's codec, used only to script
// FakeTransport replies and to compute expected written() bytes. Mirrors
// (deliberately duplicated, per this codebase's existing per-test-file
// pattern -- see tests/test_adbclient.cpp) the small helper set needed
// here.
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

std::string encodeListDone() {
    return std::string("DONE") + std::string(16, '\0');
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

// The scripted reply to the "is this actually a directory?" STAT that
// listDirectory issues before every LIST (PluginCore::checkIsDirectory):
// a plain directory. adbd answers a failed opendir() with an empty DONE
// rather than a FAIL, so that STAT is the only thing standing between a
// typo and a silently blank panel -- which means nearly every listing
// test needs one queued ahead of its LIST.
std::string dirStatScript() {
    return std::string("OKAY" "OKAY") + "STAT" + encodeStatBody(0040755, 4096, 1600000000);
}

// The scripted reply to a STAT that finds an ordinary file.
std::string fileStatScript() {
    return std::string("OKAY" "OKAY") + "STAT" + encodeStatBody(0100644, 1234, 1600000000);
}

void pushDirStat(QueueFactory* factory) {
    factory->push(std::make_unique<RecordingTransport>(dirStatScript(), nullptr, nullptr));
}

// The brief's own example: a space, an apostrophe and Cyrillic, all in one
// path -- proof the quoting holds end to end.
const char* const TRICKY_REMOTE_PATH = "/sdcard/DCIM/\xD0\x92\xD0\xB0\xD1\x81\xD1\x8F's photo.jpg";

} // namespace

// ---------------------------------------------------------------------
// Root listing ("/" -- one entry per usable device)
// ---------------------------------------------------------------------

TEST(RootListingSuite, threeDeviceFixtureListsOnlyUsableDeviceWithWarning) {
    std::string devicesText =
        "27281FDH2008DM         device usb:34603008X product:panther model:Pixel_7 "
        "device:panther transport_id:28\n"
        "emulator-5554          offline\n"
        "BADCAFE                unauthorized\n";
    std::string script = "OKAY" + hexLen4(devicesText.size()) + devicesText;

    auto transport = std::make_unique<RecordingTransport>(script, nullptr, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));
    PluginCore core(client);

    std::vector<FindResult> entries;
    std::string warning;
    std::string error;
    bool ok = core.listDirectory("/", &entries, &warning, &error);

    CHECK(ok);
    CHECK(error.empty());
    CHECK_EQ(entries.size(), static_cast<size_t>(1));
    CHECK_STR_EQ(entries[0].name, "Pixel_7 (27281FDH2008DM)");
    CHECK(entries[0].isDir);

    CHECK(!warning.empty());
    CHECK(warning.find("emulator-5554") != std::string::npos);
    CHECK(warning.find("BADCAFE") != std::string::npos);
}

// ---------------------------------------------------------------------
// displayNameForDevice / serialFromDisplayName
// ---------------------------------------------------------------------

TEST(DisplayNameSuite, roundTripWithModel) {
    DeviceInfo d;
    d.serial = "27281FDH2008DM";
    d.model = "Pixel_7";

    std::string name = PluginCore::displayNameForDevice(d);
    CHECK_STR_EQ(name, "Pixel_7 (27281FDH2008DM)");
    CHECK_STR_EQ(PluginCore::serialFromDisplayName(name), d.serial);
}

TEST(DisplayNameSuite, roundTripWithoutModel) {
    DeviceInfo d;
    d.serial = "BADCAFE";
    d.model = "";

    std::string name = PluginCore::displayNameForDevice(d);
    CHECK_STR_EQ(name, "BADCAFE");
    CHECK_STR_EQ(PluginCore::serialFromDisplayName(name), d.serial);
}

TEST(DisplayNameSuite, serialFromDisplayNameExactCaseFromBrief) {
    CHECK_STR_EQ(PluginCore::serialFromDisplayName("Pixel_7 (27281FDH2008DM)"), "27281FDH2008DM");
}

// ---------------------------------------------------------------------
// Every operation must resolve a "<model> (<serial>)" WFX path component
// back to the bare serial before talking to AdbClient -- listDirectory("/")
// is exactly what names root entries that way, so every other method has
// to undo it or no device with a known model could ever be opened. One
// test per operation family (list, get, put, one shell mutation) drives a
// display-name path all the way through and checks the literal
// host:transport:<serial> bytes sent -- not the serial PluginCore was
// merely given, but the one it actually put on the wire.
// ---------------------------------------------------------------------

TEST(DisplayNamePathSuite, listDirectoryResolvesDisplayNameToSerial) {
    std::string serial = "27281FDH2008DM";
    std::string displayPath = "/Pixel_7 (" + serial + ")/sdcard";
    std::string devicePath = "/sdcard";

    std::string script = "OKAY" "OKAY" + encodeDent(0100644, 10, 1600000000, "a.txt") +
                          encodeListDone();
    std::string statWritten;
    std::string written;
    QueueFactory factory;
    factory.push(std::make_unique<RecordingTransport>(dirStatScript(), &statWritten, nullptr));
    factory.push(std::make_unique<RecordingTransport>(script, &written, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::vector<FindResult> entries;
    std::string warning;
    std::string error;
    CHECK(core.listDirectory(displayPath, &entries, &warning, &error));
    CHECK(error.empty());

    // Both round trips must address the bare serial, not the display name.
    std::string expectedStat = encodeHostRequest("host:transport:" + serial) +
                                encodeHostRequest("sync:") +
                                syncHeaderBytes("STAT", static_cast<uint32_t>(devicePath.size())) +
                                devicePath;
    CHECK_STR_EQ(statWritten, expectedStat);

    std::string expectedWritten = encodeHostRequest("host:transport:" + serial) +
                                   encodeHostRequest("sync:") +
                                   syncHeaderBytes("LIST", static_cast<uint32_t>(devicePath.size())) +
                                   devicePath;
    CHECK_STR_EQ(written, expectedWritten);
}

TEST(DisplayNamePathSuite, getFileResolvesDisplayNameToSerial) {
    std::string serial = "27281FDH2008DM";
    TestTempDir dir;
    std::string localPath = dir.path() + "/photo.jpg";
    std::string content = "abc";

    std::string statScript = "OKAY" "OKAY" "STAT" +
        encodeStatBody(0100644, static_cast<uint32_t>(content.size()), 1600000000);
    std::string recvScript = "OKAY" "OKAY" +
        syncHeaderBytes("DATA", static_cast<uint32_t>(content.size())) + content +
        syncHeaderBytes("DONE", 0);

    QueueFactory factory;
    std::string statWritten;
    std::string recvWritten;
    factory.push(std::make_unique<RecordingTransport>(statScript, &statWritten, nullptr));
    factory.push(std::make_unique<RecordingTransport>(recvScript, &recvWritten, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    ProgressFn progress = [](uint64_t, uint64_t) { return true; };
    std::string error;
    int result = core.getFile("/Pixel_7 (" + serial + ")/sdcard/photo.jpg", localPath, 0, progress,
                               &error);

    CHECK_EQ(result, FS_FILE_OK);
    std::string expectedTransportRequest = encodeHostRequest("host:transport:" + serial);
    CHECK_EQ(statWritten.compare(0, expectedTransportRequest.size(), expectedTransportRequest), 0);
    CHECK_EQ(recvWritten.compare(0, expectedTransportRequest.size(), expectedTransportRequest), 0);
}

TEST(DisplayNamePathSuite, putFileResolvesDisplayNameToSerial) {
    std::string serial = "27281FDH2008DM";
    TestTempDir dir;
    std::string localPath = dir.path() + "/upload.bin";
    writeFile(localPath, "xyz");

    std::string script = "OKAY" "OKAY" + syncHeaderBytes("OKAY", 0);
    std::string written;
    auto transport = std::make_unique<RecordingTransport>(script, &written, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));
    PluginCore core(client);

    ProgressFn progress = [](uint64_t, uint64_t) { return true; };
    std::string error;
    int result = core.putFile(localPath, "/Pixel_7 (" + serial + ")/sdcard/upload.bin",
                               FS_COPYFLAGS_OVERWRITE, progress, &error);

    CHECK_EQ(result, FS_FILE_OK);
    std::string expectedTransportRequest = encodeHostRequest("host:transport:" + serial);
    CHECK_EQ(written.compare(0, expectedTransportRequest.size(), expectedTransportRequest), 0);
}

TEST(DisplayNamePathSuite, deleteFileResolvesDisplayNameToSerial) {
    std::string serial = "27281FDH2008DM";
    std::string wfxPath = "/Pixel_7 (" + serial + ")/sdcard/DCIM/a.jpg";
    std::string command = "rm -f " + shellQuote("/sdcard/DCIM/a.jpg");

    std::string written;
    auto transport = std::make_unique<RecordingTransport>(std::string("OKAY" "OKAY"), &written, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));
    PluginCore core(client);

    std::string error;
    CHECK(core.deleteFile(wfxPath, &error));

    std::string expectedWritten =
        encodeHostRequest("host:transport:" + serial) + encodeHostRequest("shell:" + command);
    CHECK_STR_EQ(written, expectedWritten);
}

// ---------------------------------------------------------------------
// Directory listing, caching and cache invalidation
// ---------------------------------------------------------------------

TEST(ListDirectorySuite, mapsDentModesToIsDirAndDropsDotEntries) {
    std::string script = "OKAY" "OKAY" +
                          encodeDent(0040755, 0, 1600000000, ".") +
                          encodeDent(0040755, 0, 1600000000, "..") +
                          encodeDent(0100644, 1234, 1600000001, "a.jpg") +
                          encodeDent(0040755, 0, 1600000002, "sub") +
                          encodeDent(0120777, 0, 1600000003, "link") +
                          encodeListDone();

    // "link" is a symlink, so listDirectory re-stats it through "/." to
    // decide whether it is navigable; here it resolves to a regular file.
    std::string statScript = std::string("OKAY" "OKAY") + "STAT" +
                              encodeStatBody(0100644, 99, 1600000004);

    QueueFactory factory;
    pushDirStat(&factory);
    factory.push(std::make_unique<RecordingTransport>(script, nullptr, nullptr));
    factory.push(std::make_unique<RecordingTransport>(statScript, nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::vector<FindResult> entries;
    std::string warning;
    std::string error;
    bool ok = core.listDirectory("/SERIAL1/sdcard/DCIM", &entries, &warning, &error);

    CHECK(ok);
    CHECK(error.empty());
    CHECK_EQ(entries.size(), static_cast<size_t>(3));

    CHECK_STR_EQ(entries[0].name, "a.jpg");
    CHECK(!entries[0].isDir);
    CHECK_EQ(entries[0].size, static_cast<uint64_t>(1234));
    CHECK_EQ(entries[0].mtime, static_cast<int64_t>(1600000001));

    CHECK_STR_EQ(entries[1].name, "sub");
    CHECK(entries[1].isDir);

    // Symlinks are shown as their own type: not a directory, not crashed
    // on, not silently dropped.
    CHECK_STR_EQ(entries[2].name, "link");
    CHECK(!entries[2].isDir);
    CHECK_EQ(entries[2].unixMode, static_cast<uint32_t>(0120777));
}

// ---------------------------------------------------------------------
// Symlink classification.
//
// adbd's do_list/do_stat both use lstat, so on every modern Android
// device the entries that matter most -- /sdcard above all, plus /etc,
// /d, /vendor, /bin, /odm, /product -- arrive with mode 0120777 and
// DirEntry::isDir() false. Rendering them as 21-byte files makes the
// single most common destination on the phone un-openable. listDirectory
// therefore re-stats each symlink through a trailing "/." (lstat on a
// path ending in "/." resolves through the link) and classifies the
// entry by what the link points AT. Symlinks are still never *followed*
// for transfer purposes -- this only fixes navigation.
// ---------------------------------------------------------------------

TEST(SymlinkSuite, symlinkToDirectoryIsListedAsADirectory) {
    std::string serial = "SERIAL1";
    std::string listScript = "OKAY" "OKAY" +
                              encodeDent(0120777, 21, 1600000000, "sdcard") +
                              encodeListDone();
    // lstat("/sdcard/.") resolves through the link and lands on the real
    // directory it points at.
    std::string statScript = std::string("OKAY" "OKAY") + "STAT" +
                              encodeStatBody(0040771, 4096, 1600000009);

    QueueFactory factory;
    std::string statWritten;
    pushDirStat(&factory);
    factory.push(std::make_unique<RecordingTransport>(listScript, nullptr, nullptr));
    factory.push(std::make_unique<RecordingTransport>(statScript, &statWritten, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::vector<FindResult> entries;
    std::string warning;
    std::string error;
    CHECK(core.listDirectory("/" + serial, &entries, &warning, &error));

    CHECK_EQ(entries.size(), static_cast<size_t>(1));
    CHECK_STR_EQ(entries[0].name, "sdcard");
    CHECK(entries[0].isDir);
    // The mode still describes the link itself -- only the navigability
    // decision changed.
    CHECK_EQ(entries[0].unixMode, static_cast<uint32_t>(0120777));

    std::string resolvedPath = "/sdcard/.";
    std::string expectedStat = encodeHostRequest("host:transport:" + serial) +
                                encodeHostRequest("sync:") +
                                syncHeaderBytes("STAT", static_cast<uint32_t>(resolvedPath.size())) +
                                resolvedPath;
    CHECK_STR_EQ(statWritten, expectedStat);
    CHECK_EQ(factory.callCount(), 3); // directory STAT, LIST, symlink STAT
}

TEST(SymlinkSuite, symlinkToRegularFileStaysAFile) {
    std::string listScript = "OKAY" "OKAY" +
                              encodeDent(0120777, 11, 1600000000, "link") +
                              encodeListDone();
    std::string statScript = std::string("OKAY" "OKAY") + "STAT" +
                              encodeStatBody(0100644, 4096, 1600000009);

    QueueFactory factory;
    pushDirStat(&factory);
    factory.push(std::make_unique<RecordingTransport>(listScript, nullptr, nullptr));
    factory.push(std::make_unique<RecordingTransport>(statScript, nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::vector<FindResult> entries;
    std::string warning;
    std::string error;
    CHECK(core.listDirectory("/SERIAL1/sdcard", &entries, &warning, &error));

    CHECK_EQ(entries.size(), static_cast<size_t>(1));
    CHECK_STR_EQ(entries[0].name, "link");
    CHECK(!entries[0].isDir);
}

TEST(SymlinkSuite, brokenSymlinkStaysAFileAndDoesNotFailTheListing) {
    std::string listScript = "OKAY" "OKAY" +
                              encodeDent(0120777, 7, 1600000000, "dangling") +
                              encodeListDone();
    // All-zero STAT body: the sync protocol's "does not exist".
    std::string statScript = std::string("OKAY" "OKAY") + "STAT" + encodeStatBody(0, 0, 0);

    QueueFactory factory;
    pushDirStat(&factory);
    factory.push(std::make_unique<RecordingTransport>(listScript, nullptr, nullptr));
    factory.push(std::make_unique<RecordingTransport>(statScript, nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::vector<FindResult> entries;
    std::string warning;
    std::string error;
    CHECK(core.listDirectory("/SERIAL1/sdcard", &entries, &warning, &error));

    CHECK(error.empty());
    CHECK_EQ(entries.size(), static_cast<size_t>(1));
    CHECK(!entries[0].isDir);
}

TEST(SymlinkSuite, resolutionIsCachedWithTheListingAndNotRepeated) {
    std::string listScript = "OKAY" "OKAY" +
                              encodeDent(0120777, 21, 1600000000, "sdcard") +
                              encodeListDone();
    std::string statScript = std::string("OKAY" "OKAY") + "STAT" +
                              encodeStatBody(0040771, 4096, 1600000009);

    QueueFactory factory;
    pushDirStat(&factory);
    factory.push(std::make_unique<RecordingTransport>(listScript, nullptr, nullptr));
    factory.push(std::make_unique<RecordingTransport>(statScript, nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::vector<FindResult> first;
    std::vector<FindResult> second;
    std::string warning;
    std::string error;
    CHECK(core.listDirectory("/SERIAL1", &first, &warning, &error));
    CHECK_EQ(factory.callCount(), 3);

    CHECK(core.listDirectory("/SERIAL1", &second, &warning, &error));
    CHECK_EQ(factory.callCount(), 3); // served from cache: no round trip at all
    CHECK_EQ(second.size(), static_cast<size_t>(1));
    CHECK(second[0].isDir);
}

TEST(SymlinkSuite, getFileSizesAndStampsFromTheSymlinkTargetNotTheLink) {
    TestTempDir dir;
    std::string localPath = dir.path() + "/downloaded.bin";
    std::string content = "the target's real contents";
    int64_t linkMtime = 1600000000;   // the link's own mtime -- must NOT be used
    int64_t targetMtime = 1434894309; // 2015-06-21 13:45:09 UTC

    // 1: STAT of the path itself -- adbd lstat()s it, so this is the LINK.
    std::string statLink = std::string("OKAY" "OKAY") + "STAT" +
                            encodeStatBody(0120777, 21, static_cast<uint32_t>(linkMtime));
    // 2: STAT of "<path>/." -- resolves through the link to the target.
    std::string statTarget = std::string("OKAY" "OKAY") + "STAT" +
                              encodeStatBody(0100644, static_cast<uint32_t>(content.size()),
                                             static_cast<uint32_t>(targetMtime));
    // 3: RECV streams the TARGET's bytes.
    std::string recvScript = std::string("OKAY" "OKAY") +
                              syncHeaderBytes("DATA", static_cast<uint32_t>(content.size())) +
                              content + syncHeaderBytes("DONE", 0);

    QueueFactory factory;
    std::string targetStatWritten;
    factory.push(std::make_unique<RecordingTransport>(statLink, nullptr, nullptr));
    factory.push(std::make_unique<RecordingTransport>(statTarget, &targetStatWritten, nullptr));
    factory.push(std::make_unique<RecordingTransport>(recvScript, nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::vector<uint64_t> progressTotals;
    ProgressFn progress = [&progressTotals](uint64_t, uint64_t total) -> bool {
        progressTotals.push_back(total);
        return true;
    };

    std::string error;
    int result = core.getFile("/SERIAL1/sdcard/link.bin", localPath, FS_COPYFLAGS_OVERWRITE,
                              progress, &error);

    CHECK_EQ(result, FS_FILE_OK);
    CHECK_STR_EQ(readFile(localPath), content);

    // The mtime stamped locally is the TARGET's, never the link's.
    struct stat st;
    CHECK_EQ(::stat(localPath.c_str(), &st), 0);
    CHECK_EQ(static_cast<int64_t>(st.st_mtime), targetMtime);

    // ... and the progress total is the target's size, not the link's 21.
    CHECK(!progressTotals.empty());
    if (!progressTotals.empty()) {
        CHECK_EQ(progressTotals[0], static_cast<uint64_t>(content.size()));
    }

    std::string resolvedPath = "/sdcard/link.bin/.";
    std::string expectedStat = encodeHostRequest("host:transport:SERIAL1") +
                                encodeHostRequest("sync:") +
                                syncHeaderBytes("STAT", static_cast<uint32_t>(resolvedPath.size())) +
                                resolvedPath;
    CHECK_STR_EQ(targetStatWritten, expectedStat);
}

TEST(SymlinkSuite, getFileOnAPlainFileMakesNoExtraStatRoundTrip) {
    TestTempDir dir;
    std::string localPath = dir.path() + "/plain.bin";
    std::string content = "ordinary";

    std::string statScript = std::string("OKAY" "OKAY") + "STAT" +
                              encodeStatBody(0100644, static_cast<uint32_t>(content.size()),
                                             1600000000);
    std::string recvScript = std::string("OKAY" "OKAY") +
                              syncHeaderBytes("DATA", static_cast<uint32_t>(content.size())) +
                              content + syncHeaderBytes("DONE", 0);

    QueueFactory factory;
    factory.push(std::make_unique<RecordingTransport>(statScript, nullptr, nullptr));
    factory.push(std::make_unique<RecordingTransport>(recvScript, nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::string error;
    CHECK_EQ(core.getFile("/SERIAL1/sdcard/plain.bin", localPath, FS_COPYFLAGS_OVERWRITE,
                          ProgressFn(), &error),
             FS_FILE_OK);
    CHECK_EQ(factory.callCount(), 2); // STAT + RECV only
}

TEST(ListDirectorySuite, secondCallHitsCacheAndMakesNoNewTransportCalls) {
    std::string script = "OKAY" "OKAY" + encodeDent(0100644, 10, 1600000000, "a.txt") +
                          encodeListDone();

    QueueFactory factory;
    pushDirStat(&factory);
    factory.push(std::make_unique<RecordingTransport>(script, nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::vector<FindResult> entries1;
    std::vector<FindResult> entries2;
    std::string warning;
    std::string error;

    CHECK(core.listDirectory("/SERIAL1/sdcard", &entries1, &warning, &error));
    CHECK_EQ(factory.callCount(), 2); // the directory STAT, then the LIST
    CHECK_EQ(entries1.size(), static_cast<size_t>(1));

    CHECK(core.listDirectory("/SERIAL1/sdcard", &entries2, &warning, &error));
    CHECK_EQ(factory.callCount(), 2); // no new transport requested: served from cache
    CHECK_EQ(entries2.size(), static_cast<size_t>(1));
    CHECK_STR_EQ(entries2[0].name, "a.txt");
}

TEST(ListDirectorySuite, mutatingOperationInvalidatesCachedListing) {
    std::string listScript = "OKAY" "OKAY" + encodeDent(0100644, 10, 1600000000, "a.txt") +
                              encodeListDone();

    QueueFactory factory;
    pushDirStat(&factory);
    factory.push(std::make_unique<RecordingTransport>(listScript, nullptr, nullptr));
    factory.push(std::make_unique<RecordingTransport>(std::string("OKAY" "OKAY"), nullptr, nullptr));
    pushDirStat(&factory);
    factory.push(std::make_unique<RecordingTransport>(listScript, nullptr, nullptr));

    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::vector<FindResult> entries;
    std::string warning;
    std::string error;

    CHECK(core.listDirectory("/SERIAL1/sdcard", &entries, &warning, &error));
    CHECK_EQ(factory.callCount(), 2);

    CHECK(core.deleteFile("/SERIAL1/sdcard/a.txt", &error));
    CHECK_EQ(factory.callCount(), 3);

    entries.clear();
    CHECK(core.listDirectory("/SERIAL1/sdcard", &entries, &warning, &error));
    CHECK_EQ(factory.callCount(), 5); // cache was invalidated: fresh STAT + LIST
}

// ---------------------------------------------------------------------
// A directory that isn't one, or isn't there.
//
// adbd's do_list sends DONE with zero DENTs when opendir() fails -- never
// FAIL -- so /data, a plain file, and a typo all used to come back as
// successful empty listings, get cached as empty, and leave Double
// Commander showing a blank panel with no message at all.
// ---------------------------------------------------------------------

TEST(ListDirectorySuite, aPathThatDoesNotExistIsAnErrorNotAnEmptyListing) {
    // All-zero STAT body: the sync protocol's "does not exist".
    std::string statScript = std::string("OKAY" "OKAY") + "STAT" + encodeStatBody(0, 0, 0);

    QueueFactory factory;
    factory.push(std::make_unique<RecordingTransport>(statScript, nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::vector<FindResult> entries;
    std::string warning;
    std::string error;
    CHECK(!core.listDirectory("/SERIAL1/sdcard/typo", &entries, &warning, &error));
    CHECK(error.find("no such file or directory") != std::string::npos);
    CHECK(error.find("/sdcard/typo") != std::string::npos);
    // No LIST was even attempted.
    CHECK_EQ(factory.callCount(), 1);
}

TEST(ListDirectorySuite, aRegularFileIsReportedAsNotADirectory) {
    std::string statScript =
        std::string("OKAY" "OKAY") + "STAT" + encodeStatBody(0100644, 1234, 1600000000);

    QueueFactory factory;
    factory.push(std::make_unique<RecordingTransport>(statScript, nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::vector<FindResult> entries;
    std::string warning;
    std::string error;
    CHECK(!core.listDirectory("/SERIAL1/sdcard/a.jpg", &entries, &warning, &error));
    CHECK(error.find("not a directory") != std::string::npos);
    CHECK_EQ(factory.callCount(), 1);
}

TEST(ListDirectorySuite, aGenuinelyEmptyDirectoryStillListsSuccessfully) {
    QueueFactory factory;
    pushDirStat(&factory);
    factory.push(std::make_unique<RecordingTransport>(
        std::string("OKAY" "OKAY") + encodeListDone(), nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::vector<FindResult> entries;
    std::string warning;
    std::string error;
    CHECK(core.listDirectory("/SERIAL1/sdcard/empty", &entries, &warning, &error));
    CHECK(error.empty());
    CHECK_EQ(entries.size(), static_cast<size_t>(0));
}

TEST(ListDirectorySuite, aSymlinkedDirectoryCanStillBeOpened) {
    // /sdcard itself: the guard must resolve the link before judging it,
    // or the single most common destination on the phone stops opening.
    std::string linkStat =
        std::string("OKAY" "OKAY") + "STAT" + encodeStatBody(0120777, 21, 1600000000);
    std::string targetStat =
        std::string("OKAY" "OKAY") + "STAT" + encodeStatBody(0040771, 4096, 1600000001);

    QueueFactory factory;
    factory.push(std::make_unique<RecordingTransport>(linkStat, nullptr, nullptr));
    std::string targetStatWritten;
    factory.push(std::make_unique<RecordingTransport>(targetStat, &targetStatWritten, nullptr));
    factory.push(std::make_unique<RecordingTransport>(
        std::string("OKAY" "OKAY") + encodeDent(0100644, 10, 1600000002, "a.txt") +
            encodeListDone(),
        nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::vector<FindResult> entries;
    std::string warning;
    std::string error;
    CHECK(core.listDirectory("/SERIAL1/sdcard", &entries, &warning, &error));
    CHECK(error.empty());
    CHECK_EQ(entries.size(), static_cast<size_t>(1));

    std::string resolved = "/sdcard/.";
    std::string expectedStat = encodeHostRequest("host:transport:SERIAL1") +
                                encodeHostRequest("sync:") +
                                syncHeaderBytes("STAT", static_cast<uint32_t>(resolved.size())) +
                                resolved;
    CHECK_STR_EQ(targetStatWritten, expectedStat);
}

// ---------------------------------------------------------------------
// Cache expiry, through PluginCore.
//
// Only this plugin's own mutations invalidated a cached listing, so a
// photo taken on the phone, or a change made through the other panel,
// never appeared -- Ctrl+R re-served the same stale vector. The clock is
// injected so this proves expiry without sleeping.
// ---------------------------------------------------------------------

TEST(ListDirectorySuite, aCachedListingIsRefetchedOnceItsTtlHasElapsed) {
    std::string listScript = "OKAY" "OKAY" + encodeDent(0100644, 10, 1600000000, "a.txt") +
                              encodeListDone();
    std::string refreshedScript = "OKAY" "OKAY" + encodeDent(0100644, 10, 1600000000, "a.txt") +
                                   encodeDent(0100644, 20, 1600000005, "b.txt") + encodeListDone();

    QueueFactory factory;
    pushDirStat(&factory);
    factory.push(std::make_unique<RecordingTransport>(listScript, nullptr, nullptr));
    pushDirStat(&factory);
    factory.push(std::make_unique<RecordingTransport>(refreshedScript, nullptr, nullptr));
    AdbClient client(factory.asFactory());

    int64_t now = 1000;
    PluginCore core(client, [&now]() { return now; }, /*cacheTtlSeconds=*/5);

    std::vector<FindResult> entries;
    std::string warning;
    std::string error;
    CHECK(core.listDirectory("/SERIAL1/sdcard", &entries, &warning, &error));
    CHECK_EQ(factory.callCount(), 2);

    now = 1004; // still inside the TTL: served from cache, no round trip
    entries.clear();
    CHECK(core.listDirectory("/SERIAL1/sdcard", &entries, &warning, &error));
    CHECK_EQ(factory.callCount(), 2);
    CHECK_EQ(entries.size(), static_cast<size_t>(1));

    now = 1006; // expired: the file added on the phone must now show up
    entries.clear();
    CHECK(core.listDirectory("/SERIAL1/sdcard", &entries, &warning, &error));
    CHECK_EQ(factory.callCount(), 4);
    CHECK_EQ(entries.size(), static_cast<size_t>(2));
    CHECK_STR_EQ(entries[1].name, "b.txt");
}

// ---------------------------------------------------------------------
// getFile
// ---------------------------------------------------------------------

TEST(GetFileSuite, downloadSetsContentsAndLocalMtimeFromRemote) {
    TestTempDir dir;
    std::string localPath = dir.path() + "/downloaded.bin";
    std::string content = "hello from the phone";
    int64_t remoteMtime = 981173106; // 2001-02-03 04:05:06 UTC

    std::string statScript = "OKAY" "OKAY" "STAT" +
        encodeStatBody(0100644, static_cast<uint32_t>(content.size()),
                       static_cast<uint32_t>(remoteMtime));
    std::string recvScript = "OKAY" "OKAY" +
        syncHeaderBytes("DATA", static_cast<uint32_t>(content.size())) + content +
        syncHeaderBytes("DONE", 0);

    QueueFactory factory;
    factory.push(std::make_unique<RecordingTransport>(statScript, nullptr, nullptr));
    factory.push(std::make_unique<RecordingTransport>(recvScript, nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    ProgressFn progress = [](uint64_t, uint64_t) { return true; };
    std::string error;
    int result = core.getFile("/SERIAL1/sdcard/photo.jpg", localPath, 0, progress, &error);

    CHECK_EQ(result, FS_FILE_OK);
    CHECK(error.empty());
    CHECK_STR_EQ(readFile(localPath), content);

    struct stat st{};
    CHECK_EQ(::stat(localPath.c_str(), &st), 0);
    // The headline requirement of the entire project: the local file's
    // mtime is the remote's, not the moment the download finished.
    CHECK_EQ(static_cast<int64_t>(st.st_mtime), remoteMtime);

    CHECK_EQ(dir.entryCount(), static_cast<size_t>(1)); // no leftover temp file
}

TEST(GetFileSuite, existingLocalFileWithoutOverwriteReturnsExistsAndLeavesFileUntouched) {
    TestTempDir dir;
    std::string localPath = dir.path() + "/existing.bin";
    std::string originalContent = "do not touch me";
    writeFile(localPath, originalContent);

    std::string statScript = "OKAY" "OKAY" "STAT" + encodeStatBody(0100644, 5, 1600000000);
    QueueFactory factory;
    factory.push(std::make_unique<RecordingTransport>(statScript, nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    ProgressFn progress = [](uint64_t, uint64_t) { return true; };
    std::string error;
    int result = core.getFile("/SERIAL1/sdcard/photo.jpg", localPath, 0, progress, &error);

    CHECK_EQ(result, FS_FILE_EXISTS);
    CHECK_STR_EQ(readFile(localPath), originalContent);
    CHECK_EQ(factory.callCount(), 1); // syncRecv must never have been attempted
    CHECK_EQ(dir.entryCount(), static_cast<size_t>(1)); // no temp file either
}

TEST(GetFileSuite, nonExistentRemoteReturnsNotFound) {
    TestTempDir dir;
    std::string localPath = dir.path() + "/missing-target.bin";

    std::string statScript = "OKAY" "OKAY" "STAT" + encodeStatBody(0, 0, 0);
    auto transport = std::make_unique<RecordingTransport>(statScript, nullptr, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));
    PluginCore core(client);

    ProgressFn progress = [](uint64_t, uint64_t) { return true; };
    std::string error;
    int result = core.getFile("/SERIAL1/sdcard/missing.jpg", localPath, 0, progress, &error);

    CHECK_EQ(result, FS_FILE_NOTFOUND);
    CHECK_EQ(dir.entryCount(), static_cast<size_t>(0));
}

TEST(GetFileSuite, cancelledDownloadReturnsUserAbortAndLeavesNoTempFile) {
    TestTempDir dir;
    std::string localPath = dir.path() + "/cancelled.bin";
    std::string chunk1(1000, 'A');
    std::string chunk2(1000, 'B');

    std::string statScript = "OKAY" "OKAY" "STAT" + encodeStatBody(0100644, 2000, 1600000000);
    std::string recvScript = "OKAY" "OKAY" +
        syncHeaderBytes("DATA", static_cast<uint32_t>(chunk1.size())) + chunk1 +
        syncHeaderBytes("DATA", static_cast<uint32_t>(chunk2.size())) + chunk2 +
        syncHeaderBytes("DONE", 0);

    QueueFactory factory;
    factory.push(std::make_unique<RecordingTransport>(statScript, nullptr, nullptr));
    factory.push(std::make_unique<RecordingTransport>(recvScript, nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    int progressCalls = 0;
    ProgressFn progress = [&](uint64_t, uint64_t) {
        ++progressCalls;
        return false;
    };
    std::string error;
    int result = core.getFile("/SERIAL1/sdcard/big.bin", localPath, 0, progress, &error);

    CHECK_EQ(result, FS_FILE_USERABORT);
    CHECK_EQ(progressCalls, 1);
    CHECK_EQ(dir.entryCount(), static_cast<size_t>(0)); // no temp file, no partial target
}

TEST(GetFileSuite, moveFlagDeletesRemoteFileAfterSuccessfulDownload) {
    TestTempDir dir;
    std::string localPath = dir.path() + "/moved.bin";
    std::string content = "abc";
    uint32_t remoteMtime = 1600000000;

    std::string statScript = "OKAY" "OKAY" "STAT" +
        encodeStatBody(0100644, static_cast<uint32_t>(content.size()), remoteMtime);
    std::string recvScript = "OKAY" "OKAY" +
        syncHeaderBytes("DATA", static_cast<uint32_t>(content.size())) + content +
        syncHeaderBytes("DONE", 0);

    QueueFactory factory;
    factory.push(std::make_unique<RecordingTransport>(statScript, nullptr, nullptr));
    factory.push(std::make_unique<RecordingTransport>(recvScript, nullptr, nullptr));
    std::string deleteWritten;
    factory.push(std::make_unique<RecordingTransport>(std::string("OKAY" "OKAY"), &deleteWritten, nullptr));

    AdbClient client(factory.asFactory());
    PluginCore core(client);

    ProgressFn progress = [](uint64_t, uint64_t) { return true; };
    std::string error;
    int result = core.getFile("/SERIAL1/sdcard/photo.jpg", localPath, FS_COPYFLAGS_MOVE, progress,
                               &error);

    CHECK_EQ(result, FS_FILE_OK);
    CHECK_EQ(factory.callCount(), 3);

    std::string expectedDeleteWritten =
        encodeHostRequest("host:transport:SERIAL1") +
        encodeHostRequest("shell:rm -f " + shellQuote("/sdcard/photo.jpg"));
    CHECK_STR_EQ(deleteWritten, expectedDeleteWritten);
}

// ---------------------------------------------------------------------
// putFile
// ---------------------------------------------------------------------

TEST(PutFileSuite, uploadSendsLocalFilesRealMtimeNotCurrentTime) {
    TestTempDir dir;
    std::string localPath = dir.path() + "/upload.bin";
    std::string content = "some file bytes to upload";
    writeFile(localPath, content);

    int64_t oldMtime = 981173106; // 2001-02-03 04:05:06 UTC -- the spec's own example
    struct timespec times[2];
    times[0].tv_sec = oldMtime;
    times[0].tv_nsec = 0;
    times[1].tv_sec = oldMtime;
    times[1].tv_nsec = 0;
    CHECK_EQ(::utimensat(AT_FDCWD, localPath.c_str(), times, 0), 0);

    std::string script = "OKAY" "OKAY" + syncHeaderBytes("OKAY", 0);
    std::string written;
    auto transport = std::make_unique<RecordingTransport>(script, &written, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));
    PluginCore core(client);

    ProgressFn progress = [](uint64_t, uint64_t) { return true; };
    std::string error;
    int result = core.putFile(localPath, "/SERIAL1/sdcard/upload.bin", FS_COPYFLAGS_OVERWRITE,
                               progress, &error);

    CHECK_EQ(result, FS_FILE_OK);
    CHECK(error.empty());

    // The spec's decisive test: the sync DONE packet's arg is exactly the
    // local file's real mtime -- never time(nullptr).
    CHECK(written.size() >= 8);
    std::string doneId(written.data() + written.size() - 8, 4);
    CHECK_STR_EQ(doneId, "DONE");
    uint32_t doneArg = readU32Le(
        reinterpret_cast<const unsigned char*>(written.data()) + written.size() - 4);
    CHECK_EQ(static_cast<int64_t>(doneArg), oldMtime);
}

TEST(PutFileSuite, existingRemoteWithoutOverwriteReturnsExistsAndSendsNothing) {
    TestTempDir dir;
    std::string localPath = dir.path() + "/upload2.bin";
    writeFile(localPath, "some content");

    std::string statScript = "OKAY" "OKAY" "STAT" + encodeStatBody(0100644, 10, 1600000000);
    QueueFactory factory;
    factory.push(std::make_unique<RecordingTransport>(statScript, nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    ProgressFn progress = [](uint64_t, uint64_t) { return true; };
    std::string error;
    int result = core.putFile(localPath, "/SERIAL1/sdcard/upload2.bin", 0, progress, &error);

    CHECK_EQ(result, FS_FILE_EXISTS);
    CHECK_EQ(factory.callCount(), 1); // syncSend must never have been attempted
}

TEST(PutFileSuite, moveFlagDeletesLocalFileAfterSuccessfulUpload) {
    TestTempDir dir;
    std::string localPath = dir.path() + "/toMove.bin";
    writeFile(localPath, "xyz");

    std::string script = "OKAY" "OKAY" + syncHeaderBytes("OKAY", 0);
    auto transport = std::make_unique<RecordingTransport>(script, nullptr, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));
    PluginCore core(client);

    ProgressFn progress = [](uint64_t, uint64_t) { return true; };
    std::string error;
    int result = core.putFile(localPath, "/SERIAL1/sdcard/toMove.bin",
                               FS_COPYFLAGS_OVERWRITE | FS_COPYFLAGS_MOVE, progress, &error);

    CHECK_EQ(result, FS_FILE_OK);
    CHECK_EQ(dir.entryCount(), static_cast<size_t>(0)); // local file removed after the move
}

TEST(PutFileSuite, cancelledUploadInvalidatesCache) {
    std::string listScript = "OKAY" "OKAY" + encodeDent(0100644, 10, 1600000000, "existing.txt") +
                              encodeListDone();

    TestTempDir dir;
    std::string localPath = dir.path() + "/cancel_upload.bin";
    writeFile(localPath, "some bytes to upload");

    QueueFactory factory;
    pushDirStat(&factory);
    factory.push(std::make_unique<RecordingTransport>(listScript, nullptr, nullptr));
    // A cancelled syncSend never reads a final reply (see
    // AdbClientSuite.syncSendCancellationStopsMidFileWithoutReportingSuccess
    // in test_adbclient.cpp), so the handshake alone is all this needs.
    factory.push(std::make_unique<RecordingTransport>(std::string("OKAY" "OKAY"), nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::vector<FindResult> entries;
    std::string warning;
    std::string error;
    CHECK(core.listDirectory("/SERIAL1/sdcard", &entries, &warning, &error));
    CHECK_EQ(factory.callCount(), 2);

    ProgressFn progress = [](uint64_t, uint64_t) { return false; }; // cancel immediately
    int result = core.putFile(localPath, "/SERIAL1/sdcard/cancel_upload.bin",
                               FS_COPYFLAGS_OVERWRITE, progress, &error);
    CHECK_EQ(result, FS_FILE_USERABORT);
    CHECK_EQ(factory.callCount(), 3);

    // A cancelled send may already have written a partial file remotely --
    // the cache must not still be serving the pre-upload listing.
    pushDirStat(&factory);
    factory.push(std::make_unique<RecordingTransport>(listScript, nullptr, nullptr));
    entries.clear();
    CHECK(core.listDirectory("/SERIAL1/sdcard", &entries, &warning, &error));
    CHECK_EQ(factory.callCount(), 5);
}

// ---------------------------------------------------------------------
// deleteFile / removeDir / makeDir / renameOrMove / setModificationTime --
// exact shell command strings, and error surfacing.
// ---------------------------------------------------------------------

TEST(ShellMutationSuite, deleteFileSendsExactQuotedRmCommand) {
    std::string serial = "SERIAL1";
    std::string wfxPath = "/" + serial + std::string(TRICKY_REMOTE_PATH);
    std::string command = "rm -f " + shellQuote(TRICKY_REMOTE_PATH);

    std::string written;
    auto transport = std::make_unique<RecordingTransport>(std::string("OKAY" "OKAY"), &written, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));
    PluginCore core(client);

    std::string error;
    CHECK(core.deleteFile(wfxPath, &error));
    CHECK(error.empty());

    std::string expectedWritten =
        encodeHostRequest("host:transport:" + serial) + encodeHostRequest("shell:" + command);
    CHECK_STR_EQ(written, expectedWritten);
}

TEST(ShellMutationSuite, deleteFileNonEmptyOutputSurfacesErrorAndReturnsFalse) {
    std::string errorText = "rm: /sdcard/missing.txt: No such file or directory\n";
    std::string script = "OKAY" "OKAY" + errorText;
    auto transport = std::make_unique<RecordingTransport>(script, nullptr, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));
    PluginCore core(client);

    std::string error;
    CHECK(!core.deleteFile("/SERIAL1/sdcard/missing.txt", &error));
    CHECK_STR_EQ(error, "rm: /sdcard/missing.txt: No such file or directory");
}

TEST(ShellMutationSuite, removeDirSendsExactQuotedRmRfCommand) {
    std::string serial = "SERIAL1";
    std::string wfxPath = "/" + serial + std::string(TRICKY_REMOTE_PATH);
    std::string command = "rm -rf " + shellQuote(TRICKY_REMOTE_PATH);

    std::string written;
    auto transport = std::make_unique<RecordingTransport>(std::string("OKAY" "OKAY"), &written, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));
    PluginCore core(client);

    std::string error;
    CHECK(core.removeDir(wfxPath, &error));

    std::string expectedWritten =
        encodeHostRequest("host:transport:" + serial) + encodeHostRequest("shell:" + command);
    CHECK_STR_EQ(written, expectedWritten);
}

TEST(ShellMutationSuite, removeDirNonEmptyOutputSurfacesErrorAndReturnsFalse) {
    std::string errorText = "rm: /sdcard/x: Permission denied\n";
    std::string script = "OKAY" "OKAY" + errorText;
    auto transport = std::make_unique<RecordingTransport>(script, nullptr, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));
    PluginCore core(client);

    std::string error;
    CHECK(!core.removeDir("/SERIAL1/sdcard/x", &error));
    CHECK_STR_EQ(error, "rm: /sdcard/x: Permission denied");
}

TEST(ShellMutationSuite, makeDirSendsExactQuotedMkdirCommand) {
    std::string serial = "SERIAL1";
    std::string wfxPath = "/" + serial + std::string(TRICKY_REMOTE_PATH);
    std::string command = "mkdir -p " + shellQuote(TRICKY_REMOTE_PATH);

    std::string written;
    auto transport = std::make_unique<RecordingTransport>(std::string("OKAY" "OKAY"), &written, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));
    PluginCore core(client);

    std::string error;
    CHECK(core.makeDir(wfxPath, &error));

    std::string expectedWritten =
        encodeHostRequest("host:transport:" + serial) + encodeHostRequest("shell:" + command);
    CHECK_STR_EQ(written, expectedWritten);
}

TEST(ShellMutationSuite, makeDirNonEmptyOutputSurfacesErrorAndReturnsFalse) {
    std::string errorText = "mkdir: '/sdcard/x': File exists\n";
    std::string script = "OKAY" "OKAY" + errorText;
    auto transport = std::make_unique<RecordingTransport>(script, nullptr, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));
    PluginCore core(client);

    std::string error;
    CHECK(!core.makeDir("/SERIAL1/sdcard/x", &error));
    CHECK_STR_EQ(error, "mkdir: '/sdcard/x': File exists");
}

TEST(ShellMutationSuite, renameOrMoveWithOverwriteSendsPlainMvCommand) {
    std::string serial = "SERIAL1";
    std::string fromPath = std::string(TRICKY_REMOTE_PATH);
    std::string toPath = "/sdcard/DCIM/renamed.jpg";
    std::string wfxFrom = "/" + serial + fromPath;
    std::string wfxTo = "/" + serial + toPath;
    std::string command = "mv " + shellQuote(fromPath) + " " + shellQuote(toPath);

    std::string written;
    auto transport = std::make_unique<RecordingTransport>(std::string("OKAY" "OKAY"), &written, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));
    PluginCore core(client);

    std::string error;
    CHECK(core.renameOrMove(wfxFrom, wfxTo, /*move=*/true, /*overwrite=*/true, &error));

    std::string expectedWritten =
        encodeHostRequest("host:transport:" + serial) + encodeHostRequest("shell:" + command);
    CHECK_STR_EQ(written, expectedWritten);
}

TEST(ShellMutationSuite, renameOrMoveWithoutOverwriteChecksTargetFirstThenSendsPlainMv) {
    // Task 9 review round 1: "mv -n" against an existing target exits 0
    // and prints nothing on this shell -- indistinguishable, via the
    // "empty output means success" check, from a rename that actually
    // happened. So a non-overwriting rename must stat the target first
    // and never rely on -n at all; when the target does not exist, this
    // becomes a plain "mv" (no -n) after that check passes.
    std::string serial = "SERIAL1";
    std::string fromPath = std::string(TRICKY_REMOTE_PATH);
    std::string toPath = "/sdcard/DCIM/renamed.jpg";
    std::string wfxFrom = "/" + serial + fromPath;
    std::string wfxTo = "/" + serial + toPath;
    std::string command = "mv " + shellQuote(fromPath) + " " + shellQuote(toPath);

    std::string statScript = "OKAY" "OKAY" "STAT" + encodeStatBody(0, 0, 0); // target absent

    std::string mvWritten;
    QueueFactory factory;
    factory.push(std::make_unique<RecordingTransport>(statScript, nullptr, nullptr));
    factory.push(std::make_unique<RecordingTransport>(std::string("OKAY" "OKAY"), &mvWritten, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::string error;
    CHECK(core.renameOrMove(wfxFrom, wfxTo, /*move=*/false, /*overwrite=*/false, &error));
    CHECK_EQ(factory.callCount(), 2);

    std::string expectedWritten =
        encodeHostRequest("host:transport:" + serial) + encodeHostRequest("shell:" + command);
    CHECK_STR_EQ(mvWritten, expectedWritten);
}

TEST(ShellMutationSuite, renameOrMoveWithoutOverwriteAndExistingTargetFailsWithoutRunningMv) {
    // The regression this guards against: previously "mv -n" ran
    // unconditionally and its empty, exit-0 output on a refused
    // overwrite was reported as success, silently leaving the file at
    // its old name. The existence check must catch this before any mv
    // is ever attempted.
    std::string wfxFrom = "/SERIAL1/sdcard/a.jpg";
    std::string wfxTo = "/SERIAL1/sdcard/existing.jpg";
    std::string statScript = "OKAY" "OKAY" "STAT" + encodeStatBody(0100644, 10, 1600000000); // exists

    QueueFactory factory;
    factory.push(std::make_unique<RecordingTransport>(statScript, nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::string error;
    bool ok = core.renameOrMove(wfxFrom, wfxTo, /*move=*/false, /*overwrite=*/false, &error);

    CHECK(!ok);
    CHECK(!error.empty());
    CHECK_EQ(factory.callCount(), 1); // mv must never have been attempted
}

TEST(ShellMutationSuite, renameOrMoveNonEmptyOutputSurfacesErrorAndReturnsFalse) {
    std::string errorText = "mv: can't rename '/a': No such file or directory\n";
    std::string script = "OKAY" "OKAY" + errorText;
    auto transport = std::make_unique<RecordingTransport>(script, nullptr, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));
    PluginCore core(client);

    std::string error;
    CHECK(!core.renameOrMove("/SERIAL1/a", "/SERIAL1/b", true, true, &error));
    CHECK_STR_EQ(error, "mv: can't rename '/a': No such file or directory");
}

TEST(ShellMutationSuite, renameOrMoveAcrossDevicesIsRejectedWithoutWritingAnyBytes) {
    // The sync/shell protocol is scoped to one host:transport: device at a
    // time; a move from one device's WFX path to another's has no wire
    // primitive to build on (that would need a download-then-upload,
    // which is out of scope) and must be rejected outright -- never run
    // silently on the wrong device.
    bool factoryCalled = false;
    TransportFactory factory = [&](std::string*) -> std::unique_ptr<Transport> {
        factoryCalled = true;
        return nullptr;
    };
    AdbClient client(factory);
    PluginCore core(client);

    std::string error;
    bool ok = core.renameOrMove("/DEVICE_A/sdcard/a.jpg", "/DEVICE_B/sdcard/DCIM/a.jpg", true, true,
                                 &error);

    CHECK(!ok);
    CHECK_STR_EQ(error, "cannot move between devices");
    CHECK(!factoryCalled); // no shell command was ever attempted
}

TEST(ShellMutationSuite, renameOrMoveAcrossDevicesSetsCrossDeviceOutParam) {
    // fsplugin.cpp (Task 9) needs to tell a cross-device rejection --
    // where DC's own copy+delete fallback can actually succeed -- apart
    // from every other failure, where retrying via that same fallback
    // would just waste a full download+upload+delete on a doomed
    // operation. crossDevice is that signal.
    TransportFactory factory = [](std::string*) -> std::unique_ptr<Transport> {
        return nullptr; // never reached: rejected before any transport is opened
    };
    AdbClient client(factory);
    PluginCore core(client);

    std::string error;
    bool crossDevice = false;
    bool ok = core.renameOrMove("/DEVICE_A/a", "/DEVICE_B/b", true, true, &error, &crossDevice);

    CHECK(!ok);
    CHECK(crossDevice);
}

TEST(ShellMutationSuite, renameOrMoveGenericFailureLeavesCrossDeviceFalse) {
    std::string errorText = "mv: can't rename '/a': No such file or directory\n";
    std::string script = "OKAY" "OKAY" + errorText;
    auto transport = std::make_unique<RecordingTransport>(script, nullptr, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));
    PluginCore core(client);

    std::string error;
    bool crossDevice = true; // deliberately pre-set to the wrong value
    bool ok = core.renameOrMove("/SERIAL1/a", "/SERIAL1/b", true, true, &error, &crossDevice);

    CHECK(!ok);
    CHECK(!crossDevice);
}

TEST(ShellMutationSuite, renameOrMoveWithoutOverwriteAndExistingTargetSetsTargetExistsOutParam) {
    // Task 9 review round 2: FsRenMovFileW needs to map a refused
    // overwrite to FS_FILE_EXISTS specifically (the SDK code the getFile/
    // putFile overwrite checks already use), not the generic
    // FS_FILE_WRITEERROR a plain bool can't distinguish from any other
    // failure. targetExists is that signal.
    std::string wfxFrom = "/SERIAL1/sdcard/a.jpg";
    std::string wfxTo = "/SERIAL1/sdcard/existing.jpg";
    std::string statScript = "OKAY" "OKAY" "STAT" + encodeStatBody(0100644, 10, 1600000000); // exists

    QueueFactory factory;
    factory.push(std::make_unique<RecordingTransport>(statScript, nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::string error;
    bool targetExists = false;
    bool ok = core.renameOrMove(wfxFrom, wfxTo, /*move=*/false, /*overwrite=*/false, &error,
                                /*crossDevice=*/nullptr, &targetExists);

    CHECK(!ok);
    CHECK(targetExists);
}

TEST(ShellMutationSuite, renameOrMoveGenericFailureLeavesTargetExistsFalse) {
    std::string errorText = "mv: can't rename '/a': No such file or directory\n";
    std::string script = "OKAY" "OKAY" + errorText;
    auto transport = std::make_unique<RecordingTransport>(script, nullptr, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));
    PluginCore core(client);

    std::string error;
    bool targetExists = true; // deliberately pre-set to the wrong value
    bool ok = core.renameOrMove("/SERIAL1/a", "/SERIAL1/b", true, true, &error,
                                /*crossDevice=*/nullptr, &targetExists);

    CHECK(!ok);
    CHECK(!targetExists);
}

TEST(ShellMutationSuite, renameOrMoveCrossDeviceLeavesTargetExistsFalse) {
    TransportFactory factory = [](std::string*) -> std::unique_ptr<Transport> {
        return nullptr; // never reached: rejected before any transport is opened
    };
    AdbClient client(factory);
    PluginCore core(client);

    std::string error;
    bool targetExists = true; // deliberately pre-set to the wrong value
    bool ok = core.renameOrMove("/DEVICE_A/a", "/DEVICE_B/b", true, true, &error,
                                /*crossDevice=*/nullptr, &targetExists);

    CHECK(!ok);
    CHECK(!targetExists);
}

TEST(ShellMutationSuite, setModificationTimeSendsExactQuotedTouchDCommand) {
    std::string serial = "SERIAL1";
    std::string path = std::string(TRICKY_REMOTE_PATH);
    std::string wfxPath = "/" + serial + path;
    int64_t mtime = 1434894309; // 2015-06-21 13:45:09 UTC
    std::string command = "touch -c -d @" + std::to_string(mtime) + " " + shellQuote(path);

    std::string written;
    QueueFactory factory;
    // touch -c is silent about a file that does not exist, so
    // setModificationTime STATs the path first (see
    // setModificationTimeRefusesAPathThatDoesNotExist below).
    factory.push(std::make_unique<RecordingTransport>(fileStatScript(), nullptr, nullptr));
    factory.push(std::make_unique<RecordingTransport>(std::string("OKAY" "OKAY"), &written, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::string error;
    CHECK(core.setModificationTime(wfxPath, mtime, &error));

    std::string expectedWritten =
        encodeHostRequest("host:transport:" + serial) + encodeHostRequest("shell:" + command);
    CHECK_STR_EQ(written, expectedWritten);
}

TEST(ShellMutationSuite, setModificationTimeFallsBackToDashTFormWhenDashDFails) {
    std::string serial = "SERIAL1";
    std::string path = "/sdcard/DCIM/a.jpg";
    std::string wfxPath = "/" + serial + path;
    int64_t mtime = 1000000000; // 2001-09-09 01:46:40 UTC

    std::string dCommand = "touch -c -d @" + std::to_string(mtime) + " " + shellQuote(path);
    // TZ=UTC is load-bearing, not decoration: POSIX touch -t reads its
    // argument in the SHELL's local time, and that shell is on the phone.
    // formatTouchTArg formats in UTC (gmtime_r), so without the TZ
    // assignment prefix the stamp lands off by the phone's UTC offset --
    // on the one export this whole project exists to protect.
    std::string tCommand = "TZ=UTC touch -c -t 200109090146.40 " + shellQuote(path);

    QueueFactory factory;
    std::string written1;
    std::string written2;
    factory.push(std::make_unique<RecordingTransport>(fileStatScript(), nullptr, nullptr));
    factory.push(std::make_unique<RecordingTransport>(
        std::string("OKAY" "OKAY") + "touch: unrecognized option '-d'\n", &written1, nullptr));
    factory.push(std::make_unique<RecordingTransport>(std::string("OKAY" "OKAY"), &written2, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::string error;
    CHECK(core.setModificationTime(wfxPath, mtime, &error));
    CHECK(error.empty());

    std::string expected1 =
        encodeHostRequest("host:transport:" + serial) + encodeHostRequest("shell:" + dCommand);
    std::string expected2 =
        encodeHostRequest("host:transport:" + serial) + encodeHostRequest("shell:" + tCommand);
    CHECK_STR_EQ(written1, expected1);
    CHECK_STR_EQ(written2, expected2);
}

TEST(ShellMutationSuite, setModificationTimeReturnsFalseWhenBothFormsFail) {
    std::string wfxPath = "/SERIAL1/sdcard/a.jpg";

    QueueFactory factory;
    factory.push(std::make_unique<RecordingTransport>(fileStatScript(), nullptr, nullptr));
    factory.push(std::make_unique<RecordingTransport>(
        std::string("OKAY" "OKAY") + "touch: unrecognized option '-d'\n", nullptr, nullptr));
    factory.push(std::make_unique<RecordingTransport>(
        std::string("OKAY" "OKAY") + "touch: still no good\n", nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::string error;
    CHECK(!core.setModificationTime(wfxPath, 1000000000, &error));
    CHECK_STR_EQ(error, "touch: still no good");
}

TEST(ShellMutationSuite, setModificationTimeRefusesAPathThatDoesNotExist) {
    // `touch -c` exits 0 and prints nothing for a file that isn't there,
    // and this transport gives no exit status -- so without an existence
    // check, setModificationTime cheerfully reported success for a path
    // it never touched.
    QueueFactory factory;
    factory.push(std::make_unique<RecordingTransport>(
        std::string("OKAY" "OKAY") + "STAT" + encodeStatBody(0, 0, 0), nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::string error;
    CHECK(!core.setModificationTime("/SERIAL1/sdcard/gone.jpg", 1434894309, &error));
    CHECK(error.find("no such file or directory") != std::string::npos);
    CHECK_EQ(factory.callCount(), 1); // no touch was attempted at all
}

// ---------------------------------------------------------------------
// FS_COPYFLAGS_RESUME.
//
// Double Commander only offers Resume when a plugin has answered a copy
// with FS_FILE_EXISTSRESUMEALLOWED, which this one never does -- so the
// flag is latent rather than reachable today. Answering it with
// FS_FILE_NOTSUPPORTED is still the only honest answer: neither the sync
// protocol's RECV nor its SEND has an offset, so a "resume" here would
// silently restart from zero and overwrite whatever was already there.
// ---------------------------------------------------------------------

TEST(ResumeSuite, getFileRejectsResumeWithoutTouchingTheDeviceOrTheLocalFile) {
    TestTempDir dir;
    std::string localPath = dir.path() + "/partial.bin";
    writeFile(localPath, "already here");

    QueueFactory factory; // deliberately empty: nothing may be requested
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::string error;
    int result = core.getFile("/SERIAL1/sdcard/a.bin", localPath,
                              FS_COPYFLAGS_OVERWRITE | FS_COPYFLAGS_RESUME, ProgressFn(), &error);

    CHECK_EQ(result, FS_FILE_NOTSUPPORTED);
    CHECK_EQ(factory.callCount(), 0);
    CHECK_STR_EQ(readFile(localPath), "already here"); // untouched
}

TEST(ResumeSuite, putFileRejectsResumeWithoutTouchingTheDevice) {
    TestTempDir dir;
    std::string localPath = dir.path() + "/source.bin";
    writeFile(localPath, "source bytes");

    QueueFactory factory;
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::string error;
    int result = core.putFile(localPath, "/SERIAL1/sdcard/a.bin",
                              FS_COPYFLAGS_OVERWRITE | FS_COPYFLAGS_RESUME, ProgressFn(), &error);

    CHECK_EQ(result, FS_FILE_NOTSUPPORTED);
    CHECK_EQ(factory.callCount(), 0);
}

// ---------------------------------------------------------------------
// formatTouchTArg's out-of-range guard (gmtime_r returning nullptr)
// ---------------------------------------------------------------------

TEST(ShellMutationSuite, formatTouchTArgSucceedsForOrdinaryEpoch) {
    std::string out = "unset";
    bool ok = plugincore_detail::formatTouchTArg(1434894309, &out); // 2015-06-21 13:45:09 UTC
    CHECK(ok);
    CHECK_STR_EQ(out, "201506211345.09");
}

TEST(ShellMutationSuite, formatTouchTArgReturnsFalseForOutOfRangeEpochWithoutTouchingOut) {
    std::string out = "unchanged";
    bool ok = plugincore_detail::formatTouchTArg(INT64_MAX, &out);
    CHECK(!ok);
    CHECK_STR_EQ(out, "unchanged"); // left untouched on failure
}

TEST(ShellMutationSuite, setModificationTimeReturnsFalseWhenFallbackTimestampIsOutOfRange) {
    std::string wfxPath = "/SERIAL1/sdcard/a.jpg";

    QueueFactory factory;
    factory.push(std::make_unique<RecordingTransport>(fileStatScript(), nullptr, nullptr));
    factory.push(std::make_unique<RecordingTransport>(
        std::string("OKAY" "OKAY") + "touch: unrecognized option '-d'\n", nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::string error;
    CHECK(!core.setModificationTime(wfxPath, INT64_MAX, &error));
    CHECK(!error.empty());
    // The -t fallback must never be attempted with a timestamp that can't
    // be formatted -- no further transport was requested after the STAT
    // and the -d attempt.
    CHECK_EQ(factory.callCount(), 2);
}
