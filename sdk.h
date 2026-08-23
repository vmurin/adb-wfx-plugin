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
#endif // ADB_WFX_SDK_H
