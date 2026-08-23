// Proves sdk.h is a working linkage wrapper around the vendored SDK
// headers: it must compile as C++ and expose the types/constants the
// rest of the plugin needs.
#include "sdk.h"
#include "testing.hpp"

TEST(SdkSuite, filetimeIsEightBytes) {
    CHECK_EQ(sizeof(FILETIME), static_cast<size_t>(8));
}

TEST(SdkSuite, maxPathIs260) {
    CHECK_EQ(MAX_PATH, 260);
}

TEST(SdkSuite, findDataWCanBeZeroInitAndNameWritten) {
    WIN32_FIND_DATAW findData = {};
    CHECK_EQ(findData.dwFileAttributes, static_cast<DWORD>(0));

    findData.cFileName[0] = static_cast<WCHAR>('a');
    findData.cFileName[1] = static_cast<WCHAR>('\0');
    CHECK_EQ(static_cast<int>(findData.cFileName[0]), static_cast<int>('a'));
}
