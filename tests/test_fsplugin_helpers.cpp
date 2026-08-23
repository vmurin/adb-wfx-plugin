// Tests for the fsplugin.cpp helper functions that are factored
// into fsplugin_impl.hpp per the header-only-module rule (only
// fsplugin.cpp and tests/*.cpp are translation units) so they are testable
// without dlopen'ing the built plugin.
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

TEST(TransferPercentSuite, doneExceedingTotalClampsToHundred) {
    CHECK_EQ(computeTransferPercent(200, 100), 100);
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

// ---------------------------------------------------------------------
// advanceFindData / FindHandle
// ---------------------------------------------------------------------

TEST(AdvanceFindDataSuite, skipsOverlongNameAndAdvancesIndexPastIt) {
    FindHandle handle;
    FindResult tooLong;
    tooLong.name = std::string(300, 'x');
    FindResult fits;
    fits.name = "ok.txt";
    handle.entries = {tooLong, fits};

    WIN32_FIND_DATAW findData;
    CHECK(advanceFindData(&handle, &findData));
    CHECK_STR_EQ(wideToUtf8(findData.cFileName), std::string("ok.txt"));
    CHECK_EQ(handle.index, static_cast<size_t>(2)); // both entries consumed, not just the fitting one
}

TEST(AdvanceFindDataSuite, emptyListingReturnsFalseWithoutAdvancing) {
    FindHandle handle;
    WIN32_FIND_DATAW findData;
    CHECK(!advanceFindData(&handle, &findData));
    CHECK_EQ(handle.index, static_cast<size_t>(0));
}

TEST(AdvanceFindDataSuite, successiveCallsWalkTheListingInOrderThenExhaust) {
    FindHandle handle;
    FindResult a;
    a.name = "a";
    FindResult b;
    b.name = "b";
    handle.entries = {a, b};

    WIN32_FIND_DATAW findData;
    CHECK(advanceFindData(&handle, &findData));
    CHECK_STR_EQ(wideToUtf8(findData.cFileName), std::string("a"));
    CHECK(advanceFindData(&handle, &findData));
    CHECK_STR_EQ(wideToUtf8(findData.cFileName), std::string("b"));
    CHECK(!advanceFindData(&handle, &findData));
}

// ---------------------------------------------------------------------
// isWriteOperation -- exhaustive over every FS_STATUS_OP_* constant
// (this is exactly what would have caught a missing SYNC_PUT/SYNC_DELETE
// case).
// ---------------------------------------------------------------------

TEST(IsWriteOperationSuite, exactlyTheMutatingOperationsAreWrites) {
    struct Case {
        int operation;
        bool expectedWrite;
    };
    const Case cases[] = {
        {FS_STATUS_OP_LIST, false},
        {FS_STATUS_OP_GET_SINGLE, false},
        {FS_STATUS_OP_GET_MULTI, false},
        {FS_STATUS_OP_PUT_SINGLE, true},
        {FS_STATUS_OP_PUT_MULTI, true},
        {FS_STATUS_OP_RENMOV_SINGLE, true},
        {FS_STATUS_OP_RENMOV_MULTI, true},
        {FS_STATUS_OP_DELETE, true},
        {FS_STATUS_OP_ATTRIB, true},
        {FS_STATUS_OP_MKDIR, true},
        {FS_STATUS_OP_EXEC, false},
        {FS_STATUS_OP_CALCSIZE, false},
        {FS_STATUS_OP_SEARCH, false},
        {FS_STATUS_OP_SEARCH_TEXT, false},
        {FS_STATUS_OP_SYNC_SEARCH, false},
        {FS_STATUS_OP_SYNC_GET, false},
        {FS_STATUS_OP_SYNC_PUT, true},
        {FS_STATUS_OP_SYNC_DELETE, true},
        {FS_STATUS_OP_GET_MULTI_THREAD, false},
        {FS_STATUS_OP_PUT_MULTI_THREAD, true},
    };
    for (const Case& c : cases) {
        CHECK_EQ(isWriteOperation(c.operation), c.expectedWrite);
    }
}

// ---------------------------------------------------------------------
// reportWarning / reportError / makeProgressFn -- all take the SDK
// callback pointer as an explicit parameter (rather than reading a
// fsplugin.cpp global), which is exactly what makes them testable here.
// ---------------------------------------------------------------------

namespace {

struct FakeRequestProcState {
    bool called = false;
    int pluginNr = -1;
    int requestType = -1;
    std::string title;
    std::string text;
    int maxlen = -1;
};

FakeRequestProcState gFakeRequestProcState;

BOOL DCPCALL fakeRequestProc(int pluginNr, int requestType, WCHAR* customTitle, WCHAR* customText,
                             WCHAR* /*returnedText*/, int maxlen) {
    gFakeRequestProcState.called = true;
    gFakeRequestProcState.pluginNr = pluginNr;
    gFakeRequestProcState.requestType = requestType;
    gFakeRequestProcState.title = wideToUtf8(customTitle);
    gFakeRequestProcState.text = wideToUtf8(customText);
    gFakeRequestProcState.maxlen = maxlen;
    return 0;
}

struct FakeLogProcState {
    bool called = false;
    int pluginNr = -1;
    int msgType = -1;
    std::string text;
};

FakeLogProcState gFakeLogProcState;

void DCPCALL fakeLogProc(int pluginNr, int msgType, WCHAR* logString) {
    gFakeLogProcState.called = true;
    gFakeLogProcState.pluginNr = pluginNr;
    gFakeLogProcState.msgType = msgType;
    gFakeLogProcState.text = wideToUtf8(logString);
}

struct FakeProgressProcState {
    bool called = false;
    int pluginNr = -1;
    int percent = -1;
    int returnValue = 0; // 0 == "keep going" to DC; nonzero == "abort"
};

FakeProgressProcState gFakeProgressProcState;

int DCPCALL fakeProgressProc(int pluginNr, WCHAR* /*sourceName*/, WCHAR* /*targetName*/,
                             int percentDone) {
    gFakeProgressProcState.called = true;
    gFakeProgressProcState.pluginNr = pluginNr;
    gFakeProgressProcState.percent = percentDone;
    return gFakeProgressProcState.returnValue;
}

} // namespace

TEST(ReportWarningSuite, emptyWarningDoesNotCallRequestProc) {
    gFakeRequestProcState = FakeRequestProcState{};
    reportWarning(fakeRequestProc, 7, "");
    CHECK(!gFakeRequestProcState.called);
}

TEST(ReportWarningSuite, nullRequestProcIsASafeNoOp) {
    reportWarning(static_cast<tRequestProcW>(nullptr), 7, "device offline");
    CHECK(true); // reaching this line without crashing is the assertion
}

TEST(ReportWarningSuite, nonEmptyWarningCallsRequestProcWithRTMsgOK) {
    gFakeRequestProcState = FakeRequestProcState{};
    reportWarning(fakeRequestProc, 42, "device X is offline");
    CHECK(gFakeRequestProcState.called);
    CHECK_EQ(gFakeRequestProcState.pluginNr, 42);
    CHECK_EQ(gFakeRequestProcState.requestType, RT_MsgOK);
    CHECK_STR_EQ(gFakeRequestProcState.text, std::string("device X is offline"));
}

TEST(ReportErrorSuite, emptyMessageDoesNotCallLogProc) {
    gFakeLogProcState = FakeLogProcState{};
    reportError(fakeLogProc, 1, "");
    CHECK(!gFakeLogProcState.called);
}

TEST(ReportErrorSuite, nullLogProcIsASafeNoOp) {
    reportError(static_cast<tLogProcW>(nullptr), 1, "boom");
    CHECK(true); // reaching this line without crashing is the assertion
}

TEST(ReportErrorSuite, nonEmptyMessageCallsLogProcWithImportantError) {
    gFakeLogProcState = FakeLogProcState{};
    reportError(fakeLogProc, 9, "cannot move between devices");
    CHECK(gFakeLogProcState.called);
    CHECK_EQ(gFakeLogProcState.pluginNr, 9);
    CHECK_EQ(gFakeLogProcState.msgType, MSGTYPE_IMPORTANTERROR);
    CHECK_STR_EQ(gFakeLogProcState.text, std::string("cannot move between devices"));
}

TEST(MakeProgressFnSuite, zeroReturnMeansKeepGoing) {
    // The polarity that matters most: gProgressProcW returning 0 must map
    // to ProgressFn returning true ("keep going"), not false. A
    // one-character inversion here would make Cancel mean Continue with
    // every other test in the suite still green.
    gFakeProgressProcState = FakeProgressProcState{};
    gFakeProgressProcState.returnValue = 0;
    WCHAR src[] = {static_cast<WCHAR>('a'), 0};
    WCHAR dst[] = {static_cast<WCHAR>('b'), 0};

    ProgressFn fn = makeProgressFn(fakeProgressProc, 3, src, dst);
    bool keepGoing = fn(50, 100);

    CHECK(keepGoing);
    CHECK(gFakeProgressProcState.called);
    CHECK_EQ(gFakeProgressProcState.pluginNr, 3);
    CHECK_EQ(gFakeProgressProcState.percent, 50);
}

TEST(MakeProgressFnSuite, nonZeroReturnMeansAbort) {
    gFakeProgressProcState = FakeProgressProcState{};
    gFakeProgressProcState.returnValue = 1; // DC's abort signal
    WCHAR src[] = {static_cast<WCHAR>('a'), 0};
    WCHAR dst[] = {static_cast<WCHAR>('b'), 0};

    ProgressFn fn = makeProgressFn(fakeProgressProc, 3, src, dst);
    bool keepGoing = fn(50, 100);

    CHECK(!keepGoing);
}

TEST(MakeProgressFnSuite, nullProgressProcAlwaysKeepsGoing) {
    WCHAR src[] = {static_cast<WCHAR>('a'), 0};
    WCHAR dst[] = {static_cast<WCHAR>('b'), 0};
    ProgressFn fn = makeProgressFn(static_cast<tProgressProcW>(nullptr), 3, src, dst);
    CHECK(fn(0, 100));
}

// ---------------------------------------------------------------------
// isUnsetFileTime -- FsSetTimeW's "nothing to set" guard (a zero-filled
// FILETIME means "not provided" per utils.hpp's
// fileTimeToTime, not epoch 0, and must never be forwarded as a real
// target time).
// ---------------------------------------------------------------------

TEST(IsUnsetFileTimeSuite, nullPointerIsUnset) {
    CHECK(isUnsetFileTime(nullptr));
}

TEST(IsUnsetFileTimeSuite, zeroFiletimeIsUnset) {
    FILETIME ft{};
    CHECK(isUnsetFileTime(&ft));
}

TEST(IsUnsetFileTimeSuite, nonZeroFiletimeIsSet) {
    FILETIME ft = timeToFileTime(static_cast<time_t>(1700000000));
    CHECK(!isUnsetFileTime(&ft));
}
