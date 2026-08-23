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

    auto transport = std::make_unique<RecordingTransport>(script, nullptr, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));
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

TEST(ListDirectorySuite, secondCallHitsCacheAndMakesNoNewTransportCalls) {
    std::string script = "OKAY" "OKAY" + encodeDent(0100644, 10, 1600000000, "a.txt") +
                          encodeListDone();

    QueueFactory factory;
    factory.push(std::make_unique<RecordingTransport>(script, nullptr, nullptr));
    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::vector<FindResult> entries1;
    std::vector<FindResult> entries2;
    std::string warning;
    std::string error;

    CHECK(core.listDirectory("/SERIAL1/sdcard", &entries1, &warning, &error));
    CHECK_EQ(factory.callCount(), 1);
    CHECK_EQ(entries1.size(), static_cast<size_t>(1));

    CHECK(core.listDirectory("/SERIAL1/sdcard", &entries2, &warning, &error));
    CHECK_EQ(factory.callCount(), 1); // no new transport requested: served from cache
    CHECK_EQ(entries2.size(), static_cast<size_t>(1));
    CHECK_STR_EQ(entries2[0].name, "a.txt");
}

TEST(ListDirectorySuite, mutatingOperationInvalidatesCachedListing) {
    std::string listScript = "OKAY" "OKAY" + encodeDent(0100644, 10, 1600000000, "a.txt") +
                              encodeListDone();

    QueueFactory factory;
    factory.push(std::make_unique<RecordingTransport>(listScript, nullptr, nullptr));
    factory.push(std::make_unique<RecordingTransport>(std::string("OKAY" "OKAY"), nullptr, nullptr));
    factory.push(std::make_unique<RecordingTransport>(listScript, nullptr, nullptr));

    AdbClient client(factory.asFactory());
    PluginCore core(client);

    std::vector<FindResult> entries;
    std::string warning;
    std::string error;

    CHECK(core.listDirectory("/SERIAL1/sdcard", &entries, &warning, &error));
    CHECK_EQ(factory.callCount(), 1);

    CHECK(core.deleteFile("/SERIAL1/sdcard/a.txt", &error));
    CHECK_EQ(factory.callCount(), 2);

    entries.clear();
    CHECK(core.listDirectory("/SERIAL1/sdcard", &entries, &warning, &error));
    CHECK_EQ(factory.callCount(), 3); // cache was invalidated: this made a fresh network call
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

TEST(ShellMutationSuite, renameOrMoveWithoutOverwriteUsesMvDashN) {
    std::string serial = "SERIAL1";
    std::string fromPath = std::string(TRICKY_REMOTE_PATH);
    std::string toPath = "/sdcard/DCIM/renamed.jpg";
    std::string wfxFrom = "/" + serial + fromPath;
    std::string wfxTo = "/" + serial + toPath;
    std::string command = "mv -n " + shellQuote(fromPath) + " " + shellQuote(toPath);

    std::string written;
    auto transport = std::make_unique<RecordingTransport>(std::string("OKAY" "OKAY"), &written, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));
    PluginCore core(client);

    std::string error;
    CHECK(core.renameOrMove(wfxFrom, wfxTo, /*move=*/false, /*overwrite=*/false, &error));

    std::string expectedWritten =
        encodeHostRequest("host:transport:" + serial) + encodeHostRequest("shell:" + command);
    CHECK_STR_EQ(written, expectedWritten);
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

TEST(ShellMutationSuite, setModificationTimeSendsExactQuotedTouchDCommand) {
    std::string serial = "SERIAL1";
    std::string path = std::string(TRICKY_REMOTE_PATH);
    std::string wfxPath = "/" + serial + path;
    int64_t mtime = 1434894309; // 2015-06-21 13:45:09 UTC
    std::string command = "touch -c -d @" + std::to_string(mtime) + " " + shellQuote(path);

    std::string written;
    auto transport = std::make_unique<RecordingTransport>(std::string("OKAY" "OKAY"), &written, nullptr);
    AdbClient client(singleUseFactory(std::move(transport)));
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
    std::string tCommand = "touch -c -t 200109090146.40 " + shellQuote(path);

    QueueFactory factory;
    std::string written1;
    std::string written2;
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
