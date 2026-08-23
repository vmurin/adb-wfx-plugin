// Tests for the fsplugin.cpp (Task 9) helper functions that are factored
// into fsplugin_impl.hpp per constraints.md #4 (header-only modules; only
// fsplugin.cpp and tests/*.cpp are translation units) so they are testable
// without dlopen'ing the built plugin. See task-9-brief.md's "Required
// additional test cases".
#include "fsplugin_impl.hpp"

#include "testing.hpp"

#include <cstdint>
#include <string>

// ---------------------------------------------------------------------
// computeTransferPercent
// ---------------------------------------------------------------------

TEST(TransferPercentSuite, zeroDoneOfHundredIsZero) {
    CHECK_EQ(computeTransferPercent(0, 100), 0);
}

TEST(TransferPercentSuite, halfwayIsFifty) {
    CHECK_EQ(computeTransferPercent(50, 100), 50);
}

TEST(TransferPercentSuite, doneEqualsTotalIsHundred) {
    CHECK_EQ(computeTransferPercent(100, 100), 100);
}

TEST(TransferPercentSuite, oneThirdRoundsDownToThirtyThree) {
    CHECK_EQ(computeTransferPercent(1, 3), 33);
}

TEST(TransferPercentSuite, zeroTotalIsZeroNotDivByZero) {
    CHECK_EQ(computeTransferPercent(42, 0), 0);
}

TEST(TransferPercentSuite, multiGigabyteTransferDoesNotOverflow) {
    // 5 GB of 10 GB, expressed the way the brief spells it out: done and
    // total both already past 32 bits, so done*100 alone (5e11) would
    // overflow a 32-bit intermediate. Must still come out exactly 50.
    uint64_t done = 5000000000ULL;
    uint64_t total = 10000000000ULL;
    CHECK_EQ(computeTransferPercent(done, total), 50);
}

// ---------------------------------------------------------------------
// fillFindData
// ---------------------------------------------------------------------

TEST(FillFindDataSuite, directoryGetsDirectoryAttributeAndUnixMode) {
    FindResult entry;
    entry.name = "sdcard";
    entry.isDir = true;
    entry.unixMode = 040755; // octal: drwxr-xr-x

    WIN32_FIND_DATAW findData;
    CHECK(fillFindData(entry, &findData));

    CHECK((findData.dwFileAttributes & static_cast<DWORD>(FILE_ATTRIBUTE_DIRECTORY)) != 0);
    CHECK((findData.dwFileAttributes & static_cast<DWORD>(FILE_ATTRIBUTE_UNIX_MODE)) != 0);
    CHECK_EQ(findData.dwReserved0, static_cast<DWORD>(040755));
    CHECK_STR_EQ(wideToUtf8(findData.cFileName), std::string("sdcard"));
}

TEST(FillFindDataSuite, regularFileHasNoDirectoryAttribute) {
    FindResult entry;
    entry.name = "a.jpg";
    entry.isDir = false;
    entry.unixMode = 0100644;

    WIN32_FIND_DATAW findData;
    CHECK(fillFindData(entry, &findData));

    CHECK_EQ(findData.dwFileAttributes & static_cast<DWORD>(FILE_ATTRIBUTE_DIRECTORY),
             static_cast<DWORD>(0));
}

TEST(FillFindDataSuite, fiveGigabyteFileSplitsAcrossHighAndLowSize) {
    FindResult entry;
    entry.name = "big.bin";
    entry.size = 5368709120ULL; // 5 GiB = 5 * 1024^3 = 0x1_4000_0000

    WIN32_FIND_DATAW findData;
    CHECK(fillFindData(entry, &findData));

    CHECK_EQ(findData.nFileSizeHigh, static_cast<DWORD>(1));
    CHECK_EQ(findData.nFileSizeLow, static_cast<DWORD>(0x40000000));
}

TEST(FillFindDataSuite, mtimeFillsAllThreeTimestamps) {
    FindResult entry;
    entry.name = "f.txt";
    entry.mtime = 1700000000; // arbitrary fixed epoch second

    WIN32_FIND_DATAW findData;
    CHECK(fillFindData(entry, &findData));

    FILETIME expected = timeToFileTime(static_cast<time_t>(1700000000));
    CHECK_EQ(findData.ftLastWriteTime.dwLowDateTime, expected.dwLowDateTime);
    CHECK_EQ(findData.ftLastWriteTime.dwHighDateTime, expected.dwHighDateTime);
    CHECK_EQ(findData.ftCreationTime.dwLowDateTime, expected.dwLowDateTime);
    CHECK_EQ(findData.ftCreationTime.dwHighDateTime, expected.dwHighDateTime);
    CHECK_EQ(findData.ftLastAccessTime.dwLowDateTime, expected.dwLowDateTime);
    CHECK_EQ(findData.ftLastAccessTime.dwHighDateTime, expected.dwHighDateTime);
}

TEST(FillFindDataSuite, nameOfExactlyMaxPathUnitsFitsWithRoomForNul) {
    // MAX_PATH - 1 units plus the NUL fits exactly.
    FindResult entry;
    entry.name = std::string(static_cast<size_t>(MAX_PATH - 1), 'a');

    WIN32_FIND_DATAW findData;
    CHECK(fillFindData(entry, &findData));
    CHECK_EQ(wideToUtf8(findData.cFileName).size(), static_cast<size_t>(MAX_PATH - 1));
}

TEST(FillFindDataSuite, nameOf300UnitsIsSkippedNotTruncated) {
    // 300 UTF-16 units is longer than MAX_PATH (260): the brief requires
    // this entry be skipped by the caller, not silently truncated into a
    // name that refers to something else.
    FindResult entry;
    entry.name = std::string(300, 'x');

    WIN32_FIND_DATAW findData;
    CHECK(!fillFindData(entry, &findData));
}
