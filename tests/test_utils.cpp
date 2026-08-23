// Tests for utils.hpp: UTF-16 <-> UTF-8 conversion and FILETIME <-> time_t
// arithmetic. Written before utils.hpp exists (TDD) -- see
// .superpowers/sdd/plan-adb-wfx/task-2-report.md for the RED run.
#include "utils.hpp"
#include "testing.hpp"

#include <cstdint>
#include <ctime>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

// Builds a NUL-terminated WCHAR buffer from raw code units (the trailing 0
// is added automatically), for exercising malformed UTF-16 input directly.
std::vector<WCHAR> wcharBuf(std::initializer_list<WCHAR> units) {
    std::vector<WCHAR> buf(units);
    buf.push_back(0);
    return buf;
}

} // namespace

TEST(UtilsSuite, asciiRoundTrip) {
    std::vector<WCHAR> wide = utf8ToWide("hello");
    CHECK_STR_EQ(wideToUtf8(wide.data()), "hello");
}

TEST(UtilsSuite, cyrillicRoundTrip) {
    std::string original = "Привет мир";
    std::vector<WCHAR> wide = utf8ToWide(original);
    CHECK_STR_EQ(wideToUtf8(wide.data()), original);
}

TEST(UtilsSuite, cjkRoundTrip) {
    std::string original = "日本語";
    std::vector<WCHAR> wide = utf8ToWide(original);
    CHECK_STR_EQ(wideToUtf8(wide.data()), original);
}

TEST(UtilsSuite, emojiRoundTripAndSurrogatePairUnitsAreExact) {
    std::string original = "🚀"; // U+1F680
    std::vector<WCHAR> wide = utf8ToWide(original);

    CHECK_EQ(wide.size(), static_cast<size_t>(3)); // pair + trailing NUL
    CHECK_EQ(wide[0], static_cast<WCHAR>(0xD83D));
    CHECK_EQ(wide[1], static_cast<WCHAR>(0xDE80));
    CHECK_EQ(wide[2], static_cast<WCHAR>(0));

    CHECK_STR_EQ(wideToUtf8(wide.data()), original);
}

TEST(UtilsSuite, mixedStringRoundTrip) {
    std::string original = "hello Привет 日本語 🚀/mix";
    std::vector<WCHAR> wide = utf8ToWide(original);
    CHECK_STR_EQ(wideToUtf8(wide.data()), original);
}

TEST(UtilsSuite, emptyStringRoundTrip) {
    std::vector<WCHAR> wide = utf8ToWide("");
    CHECK_EQ(wide.size(), static_cast<size_t>(1)); // trailing NUL only
    CHECK_EQ(wide[0], static_cast<WCHAR>(0));
    CHECK_STR_EQ(wideToUtf8(wide.data()), "");
    CHECK_STR_EQ(wideToUtf8(wide.data(), 0), "");
}

TEST(UtilsSuite, wideToUtf8ExplicitLengthOverload) {
    std::vector<WCHAR> wide = utf8ToWide("abc"); // {'a','b','c',0}
    CHECK_STR_EQ(wideToUtf8(wide.data(), 3), "abc");
}

TEST(UtilsSuite, utf8ToWideBufExactFitSucceeds) {
    const size_t destChars = 3; // "hi" -> 2 units + NUL
    WCHAR dest[destChars];
    bool ok = utf8ToWideBuf("hi", dest, destChars);

    CHECK(ok);
    CHECK_EQ(dest[0], static_cast<WCHAR>('h'));
    CHECK_EQ(dest[1], static_cast<WCHAR>('i'));
    CHECK_EQ(dest[2], static_cast<WCHAR>(0));
}

TEST(UtilsSuite, utf8ToWideBufOneUnitTooSmallTruncatesSurrogatePairCleanly) {
    const size_t destChars = 2; // emoji needs 2 units + NUL == 3; one short
    WCHAR dest[destChars];
    bool ok = utf8ToWideBuf("🚀", dest, destChars);

    CHECK(!ok);

    size_t nulIndex = destChars; // sentinel meaning "not found"
    for (size_t i = 0; i < destChars; ++i) {
        if (dest[i] == 0) {
            nulIndex = i;
            break;
        }
    }
    CHECK(nulIndex < destChars); // must be NUL-terminated within the buffer

    bool endsInLoneHighSurrogate = nulIndex > 0 &&
        dest[nulIndex - 1] >= 0xD800 && dest[nulIndex - 1] <= 0xDBFF;
    CHECK(!endsInLoneHighSurrogate);
}

TEST(UtilsSuite, unpairedHighSurrogateBecomesReplacementCharacter) {
    std::vector<WCHAR> lonelyHighSurrogate = wcharBuf({0xD83D});
    CHECK_STR_EQ(wideToUtf8(lonelyHighSurrogate.data()), "\xEF\xBF\xBD");
}

TEST(UtilsSuite, unpairedLowSurrogateBecomesReplacementCharacter) {
    std::vector<WCHAR> lonelyLowSurrogate = wcharBuf({0xDE80});
    CHECK_STR_EQ(wideToUtf8(lonelyLowSurrogate.data()), "\xEF\xBF\xBD");
}

TEST(UtilsSuite, invalidUtf8LeadByteBecomesReplacementCharacter) {
    std::vector<WCHAR> wide = utf8ToWide("\xFF");
    CHECK_EQ(wide.size(), static_cast<size_t>(2));
    CHECK_EQ(wide[0], static_cast<WCHAR>(0xFFFD));
    CHECK_EQ(wide[1], static_cast<WCHAR>(0));
}

TEST(UtilsSuite, truncatedMultiByteUtf8SequenceBecomesReplacementCharacter) {
    // 3-byte lead followed by only one continuation byte.
    std::vector<WCHAR> wide = utf8ToWide("\xE0\x80");
    CHECK_EQ(wide[0], static_cast<WCHAR>(0xFFFD));
}

TEST(UtilsSuite, overlongUtf8EncodingBecomesReplacementCharacter) {
    std::vector<WCHAR> wide = utf8ToWide("\xC0\x80"); // overlong NUL
    CHECK_EQ(wide[0], static_cast<WCHAR>(0xFFFD));
}

TEST(UtilsSuite, codePointAboveMaxBecomesReplacementCharacter) {
    std::vector<WCHAR> wide = utf8ToWide("\xF4\x90\x80\x80"); // U+110000
    CHECK_EQ(wide[0], static_cast<WCHAR>(0xFFFD));
}

TEST(UtilsSuite, timeToFileTimeAtEpochRecombinesToEpochConstant) {
    FILETIME ft = timeToFileTime(0);
    uint64_t recombined =
        (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    CHECK_EQ(recombined, FILETIME_AT_UNIX_EPOCH);
}

TEST(UtilsSuite, fileTimeRoundTripsThroughVariousTimes) {
    time_t samples[] = {0, 1, 1000000000, 1755000000, 2147483647, 4102444800};
    for (time_t t : samples) {
        FILETIME ft = timeToFileTime(t);
        CHECK_EQ(fileTimeToTime(ft), t);
    }
}

TEST(UtilsSuite, zeroFileTimeIsUnknownNotNegativeTime) {
    FILETIME zero = {0, 0};
    CHECK_EQ(fileTimeToTime(zero), static_cast<time_t>(0));
}

// A FILETIME earlier than the Unix epoch used to be computed as
// `ticks - FILETIME_AT_UNIX_EPOCH` in uint64_t, which wraps to something
// astronomically large and reaches the device as `touch -d @<garbage>`.
// Dates before 1970 are rare but perfectly legal (scanned documents,
// deliberately back-dated archives), and a wrong date is precisely the
// defect this project exists to eliminate.

TEST(UtilsSuite, fileTimeBeforeTheUnixEpochRoundTripsAsANegativeTime) {
    const time_t nineteenSixty = -315619200; // 1960-01-01T00:00:00Z
    FILETIME ft = timeToFileTime(nineteenSixty);
    CHECK_EQ(fileTimeToTime(ft), nineteenSixty);
}

TEST(UtilsSuite, fileTimeOneSecondBeforeTheEpochIsMinusOneNotAHugeNumber) {
    FILETIME ft = timeToFileTime(-1);
    CHECK_EQ(fileTimeToTime(ft), static_cast<time_t>(-1));
}

TEST(UtilsSuite, aFileTimeFarBelowTheEpochStaysNegative) {
    // Ticks well under FILETIME_AT_UNIX_EPOCH but not zero (zero means
    // "unset" in this SDK and is handled separately above).
    FILETIME ft;
    ft.dwLowDateTime = 1;
    ft.dwHighDateTime = 0;
    CHECK(fileTimeToTime(ft) < 0);
}
