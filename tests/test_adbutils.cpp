// Tests for adbutils.hpp: WFX remote path parsing/joining, single-quote
// shell escaping for the `shell:` service, and the directory listing
// cache. Written before adbutils.hpp exists (TDD) -- see
// .superpowers/sdd/plan-adb-wfx/task-3-report.md for the RED run.
#include "adbutils.hpp"
#include "testing.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------
// parseRemotePath
// ---------------------------------------------------------------------

TEST(ParseRemotePathSuite, emptyStringIsRoot) {
    RemotePath p = parseRemotePath("");
    CHECK(p.isRoot);
    CHECK_STR_EQ(p.serial, "");
    CHECK_STR_EQ(p.path, "/");
}

TEST(ParseRemotePathSuite, slashIsRoot) {
    RemotePath p = parseRemotePath("/");
    CHECK(p.isRoot);
    CHECK_STR_EQ(p.serial, "");
    CHECK_STR_EQ(p.path, "/");
}

TEST(ParseRemotePathSuite, bareSerialIsDeviceRoot) {
    RemotePath p = parseRemotePath("/ABC123");
    CHECK(!p.isRoot);
    CHECK_STR_EQ(p.serial, "ABC123");
    CHECK_STR_EQ(p.path, "/");
}

TEST(ParseRemotePathSuite, bareSerialWithTrailingSlashIsDeviceRoot) {
    RemotePath p = parseRemotePath("/ABC123/");
    CHECK(!p.isRoot);
    CHECK_STR_EQ(p.serial, "ABC123");
    CHECK_STR_EQ(p.path, "/");
}

TEST(ParseRemotePathSuite, serialPlusOneComponent) {
    RemotePath p = parseRemotePath("/ABC123/sdcard");
    CHECK(!p.isRoot);
    CHECK_STR_EQ(p.serial, "ABC123");
    CHECK_STR_EQ(p.path, "/sdcard");
}

TEST(ParseRemotePathSuite, serialPlusDeepPathWithSpace) {
    RemotePath p = parseRemotePath("/ABC123/sdcard/DCIM/a b.jpg");
    CHECK(!p.isRoot);
    CHECK_STR_EQ(p.serial, "ABC123");
    CHECK_STR_EQ(p.path, "/sdcard/DCIM/a b.jpg");
}

TEST(ParseRemotePathSuite, backslashIsNotASeparator) {
    RemotePath p = parseRemotePath("/ABC/a\\b");
    CHECK(!p.isRoot);
    CHECK_STR_EQ(p.serial, "ABC");
    CHECK_STR_EQ(p.path, "/a\\b");
}

// ---------------------------------------------------------------------
// joinWfxPath
// ---------------------------------------------------------------------

TEST(JoinWfxPathSuite, rootPlusSerial) {
    CHECK_STR_EQ(joinWfxPath("/", "ABC123"), "/ABC123");
}

TEST(JoinWfxPathSuite, deviceRootPlusLeaf) {
    CHECK_STR_EQ(joinWfxPath("/ABC123", "sdcard"), "/ABC123/sdcard");
}

TEST(JoinWfxPathSuite, deepDirPlusLeafWithSpace) {
    CHECK_STR_EQ(joinWfxPath("/ABC123/sdcard/DCIM", "a b.jpg"),
                 "/ABC123/sdcard/DCIM/a b.jpg");
}

// ---------------------------------------------------------------------
// shellQuote -- security-critical: hand-written expectations for the
// exact forms called out in the brief.
// ---------------------------------------------------------------------

TEST(ShellQuoteSuite, plainWord) {
    CHECK_STR_EQ(shellQuote("abc"), "'abc'");
}

TEST(ShellQuoteSuite, wordWithSpace) {
    CHECK_STR_EQ(shellQuote("a b"), "'a b'");
}

TEST(ShellQuoteSuite, embeddedApostrophe) {
    CHECK_STR_EQ(shellQuote("it's"), "'it'\\''s'");
}

TEST(ShellQuoteSuite, emptyString) {
    CHECK_STR_EQ(shellQuote(""), "''");
}

TEST(ShellQuoteSuite, dollarIsNotExpanded) {
    CHECK_STR_EQ(shellQuote("$HOME"), "'$HOME'");
}

TEST(ShellQuoteSuite, backticksAreLiteral) {
    CHECK_STR_EQ(shellQuote("`id`"), "'`id`'");
}

TEST(ShellQuoteSuite, semicolonAndFlagsAreLiteral) {
    CHECK_STR_EQ(shellQuote("a;rm -rf /"), "'a;rm -rf /'");
}

TEST(ShellQuoteSuite, realNewlineIsKeptInsideQuotes) {
    CHECK_STR_EQ(shellQuote("a\nb"), "'a\nb'");
}

TEST(ShellQuoteSuite, cyrillicFilename) {
    CHECK_STR_EQ(shellQuote("\xD0\xA4\xD0\xBE\xD1\x82\xD0\xBE.jpg"),
                 "'\xD0\xA4\xD0\xBE\xD1\x82\xD0\xBE.jpg'"); // "Фото.jpg"
}

TEST(ShellQuoteSuite, leadingDashDashIsNotTreatedAsAFlag) {
    CHECK_STR_EQ(shellQuote("--rf"), "'--rf'");
}

// ---------------------------------------------------------------------
// shellQuote -- property check: quote a table of nasty inputs, feed each
// quoted form to `/bin/sh -c "printf %s <quoted>"` via popen, and assert
// the shell hands back the original byte for byte. This is what actually
// proves the quoting is correct, rather than restating hand-written
// expectations.
// ---------------------------------------------------------------------

namespace {

std::string runThroughShell(const std::string& quotedArg) {
    std::string command = "printf %s " + quotedArg;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("popen failed");
    }
    std::string output;
    char buf[256];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), pipe)) > 0) {
        output.append(buf, n);
    }
    pclose(pipe);
    return output;
}

} // namespace

TEST(ShellQuoteSuite, popenRoundTripOnNastyInputs) {
    std::vector<std::string> nastyInputs = {
        "abc",                          // plain word
        "a b",                          // word with space
        "it's",                         // embedded apostrophe
        "",                             // empty string
        "$HOME",                        // must not expand
        "`id`",                         // must not command-substitute
        "a;rm -rf /",                   // must not be split into commands
        "a\nb",                         // real embedded newline
        "\xD0\xA4\xD0\xBE\xD1\x82\xD0\xBE.jpg", // "Фото.jpg"
        "--rf",                         // must not be parsed as an option
        "'''",                          // three consecutive single quotes
    };

    for (const std::string& input : nastyInputs) {
        std::string roundTripped = runThroughShell(shellQuote(input));
        CHECK_STR_EQ(roundTripped, input);
    }
}

// ---------------------------------------------------------------------
// DirEntry (adbproto.hpp)
// ---------------------------------------------------------------------

TEST(DirEntrySuite, isDirTrueForDirectoryMode) {
    DirEntry entry;
    entry.mode = 0040755; // regular directory, rwxr-xr-x
    CHECK(entry.isDir());
    CHECK(!entry.isSymlink());
}

TEST(DirEntrySuite, isSymlinkTrueForSymlinkMode) {
    DirEntry entry;
    entry.mode = 0120777; // symlink
    CHECK(entry.isSymlink());
    CHECK(!entry.isDir());
}

TEST(DirEntrySuite, isDirFalseForRegularFile) {
    DirEntry entry;
    entry.mode = 0100644; // regular file, rw-r--r--
    CHECK(!entry.isDir());
    CHECK(!entry.isSymlink());
}

// ---------------------------------------------------------------------
// ListingCache
// ---------------------------------------------------------------------

TEST(ListingCacheSuite, putGetRoundTrip) {
    ListingCache cache;
    std::vector<DirEntry> entries(2);
    entries[0].name = "DCIM";
    entries[1].name = "Download";

    cache.put("/S/sdcard", entries);

    const std::vector<DirEntry>* got = cache.get("/S/sdcard");
    CHECK(got != nullptr);
    CHECK_EQ(got->size(), static_cast<size_t>(2));
    CHECK_STR_EQ((*got)[0].name, "DCIM");
    CHECK_STR_EQ((*got)[1].name, "Download");
}

TEST(ListingCacheSuite, getOfAbsentKeyReturnsNullptr) {
    ListingCache cache;
    CHECK(cache.get("/nope") == nullptr);
}

TEST(ListingCacheSuite, invalidateDropsDirAndChildrenButNotSiblingsOrPrefixCollisions) {
    ListingCache cache;
    cache.put("/S/sdcard", std::vector<DirEntry>{});
    cache.put("/S/sdcard/DCIM", std::vector<DirEntry>{});
    cache.put("/S/sdcardX", std::vector<DirEntry>{});
    cache.put("/S/other", std::vector<DirEntry>{});

    cache.invalidate("/S/sdcard");

    CHECK(cache.get("/S/sdcard") == nullptr);
    CHECK(cache.get("/S/sdcard/DCIM") == nullptr);
    CHECK(cache.get("/S/sdcardX") != nullptr);
    CHECK(cache.get("/S/other") != nullptr);
}

TEST(ListingCacheSuite, clearEmptiesTheCache) {
    ListingCache cache;
    cache.put("/S/sdcard", std::vector<DirEntry>{});
    cache.put("/S/other", std::vector<DirEntry>{});
    CHECK_EQ(cache.size(), static_cast<size_t>(2));

    cache.clear();

    CHECK_EQ(cache.size(), static_cast<size_t>(0));
    CHECK(cache.get("/S/sdcard") == nullptr);
}

TEST(ListingCacheSuite, sizeReflectsNumberOfCachedDirectories) {
    ListingCache cache;
    CHECK_EQ(cache.size(), static_cast<size_t>(0));
    cache.put("/S/sdcard", std::vector<DirEntry>{});
    CHECK_EQ(cache.size(), static_cast<size_t>(1));
}
