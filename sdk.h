// The ONLY file in this project permitted to include the vendored Double
// Commander SDK headers. Wrapping them in extern "C" here keeps their
// linkage consistent across every translation unit that needs WCHAR,
// FILETIME, WIN32_FIND_DATAW, RemoteInfoStruct or the FS_* constants --
// those files must include "sdk.h" and never "wfxplugin.h" or "common.h"
// directly.
#ifndef ADB_WFX_SDK_H
#define ADB_WFX_SDK_H
extern "C" {
#include "wfxplugin.h"
}

// wfxplugin.h/common.h (both vendored, never edited -- see
// constraints.md #1) don't define this on a non-Windows build; every
// other SDK-shaped name (HANDLE, WCHAR, FILETIME, ...) already comes from
// them, so this belongs here rather than namespaced inside fsplugin.cpp,
// where a macro can't actually be scoped anyway.
#ifndef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE ((HANDLE)(-1))
#endif

#endif // ADB_WFX_SDK_H
