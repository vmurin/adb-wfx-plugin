// fsplugin.cpp -- the extern "C" export shim, and the ONLY translation
// unit besides tests/*.cpp in this project (constraints.md #4).
//
// This file owns exactly: process-global state (the callbacks DC hands
// FsInitW, the AdbClient/PluginCore pair, and the find-handle lifecycle),
// UTF-16<->UTF-8 conversion at the SDK boundary (utils.hpp), and bridging
// DC's progress/log/request callbacks to the plain C++ types PluginCore
// and AdbClient use. Every actual decision -- device naming, caching, the
// download-to-temp-then-rename dance, mtime preservation, which shell
// commands the mutating operations run -- lives in PluginCore
// (fsplugin_impl.hpp, Task 8). If a change here starts to look like a
// policy decision rather than a translation, it belongs there instead.
//
// No C++ exception may ever cross this file's extern "C" boundary -- an
// escaping exception terminates Double Commander, not just the plugin.
// Every export body is therefore `try { ... } catch (...) { return
// <the appropriate error value>; }`.
//
// Threading note: Double Commander can call a WFX plugin from more than
// one thread (its background-transfer modes). Making this file's global
// state thread-safe is out of scope for this task (see task-9-brief.md);
// the globals below are kept to exactly the ones the brief lists, and
// nothing here does anything to make concurrent use *more* broken than
// the single AdbClient/PluginCore pair already implies.
#include "sdk.h"

#include "adbclient.hpp"
#include "adbserver.hpp"
#include "fsplugin_impl.hpp"
#include "transport.hpp"
#include "utils.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

// Host and port fsplugin.cpp's TransportFactory connects to. The port
// itself is resolved per constraints.md's protocol reference
// (ANDROID_ADB_SERVER_PORT, falling back to ADB_DEFAULT_PORT) by
// adbServerPort(); only the loopback host is fixed here.
constexpr const char* ADB_SERVER_HOST = "127.0.0.1";

// Title shown on the RT_MsgOK dialog FsFindFirstW raises for a
// listDirectory warning (e.g. an unauthorized/offline device at the
// root listing).
constexpr const char* WARNING_DIALOG_TITLE = "ADB";

// The value DC's SDK headers call FsGetDefRootName's result and the WFX
// root path's first component after it: "ADB" everywhere a device serial
// or model name isn't in play yet.
constexpr const char* ROOT_NAME = "ADB";

// wfxplugin.h/common.h (both vendored, never edited -- see
// constraints.md #1) don't define these on a non-Windows build; define
// them ourselves, guarded in case a future SDK header revision does.
#ifndef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE ((HANDLE)(-1))
#endif

// -----------------------------------------------------------------
// Process-global state (see the file comment above for the threading
// caveat). FsInitW populates all five; every other export assumes they
// already exist, which the SDK's contract guarantees (DC always calls
// FsInitW before any other export).
// -----------------------------------------------------------------
int gPluginNr = 0;
tProgressProcW gProgressProcW = nullptr;
tLogProcW gLogProcW = nullptr;
tRequestProcW gRequestProcW = nullptr;
std::unique_ptr<AdbClient> gAdbClient;
std::unique_ptr<PluginCore> gPluginCore;

// One FsFindFirstW/FsFindNextW/FsFindClose cycle's state: the full
// listing (PluginCore::listDirectory has no notion of paging) plus how
// far FsFindNextW has consumed it. Returned to DC as an opaque HANDLE.
struct FindHandle {
    std::vector<FindResult> entries;
    size_t index = 0;
};

// Fills findData from the next entry in handle's listing that actually
// fits a WIN32_FIND_DATAW (fillFindData, fsplugin_impl.hpp, skips one
// whose name doesn't fit MAX_PATH rather than truncate it), advancing
// handle->index past every entry it looks at. Returns false once the
// listing is exhausted. Shared by FsFindFirstW and FsFindNextW so this
// skip-and-advance logic exists in exactly one place.
bool advanceFindData(FindHandle* handle, WIN32_FIND_DATAW* findData) {
    while (handle->index < handle->entries.size()) {
        const FindResult& entry = handle->entries[handle->index];
        ++handle->index;
        if (fillFindData(entry, findData)) {
            return true;
        }
    }
    return false;
}

// Raises warning (if non-empty) as an RT_MsgOK dialog via gRequestProcW --
// used for listDirectory's *warning output (e.g. "device unauthorized",
// see fsplugin_impl.hpp/PluginCore::listDirectory), which must reach the
// user even when the rest of the listing came back fine.
void reportWarning(const std::string& warning) {
    if (warning.empty() || gRequestProcW == nullptr) {
        return;
    }
    std::vector<WCHAR> title = utf8ToWide(WARNING_DIALOG_TITLE);
    std::vector<WCHAR> text = utf8ToWide(warning);
    WCHAR noReturnBuffer[1] = {0};
    gRequestProcW(gPluginNr, RT_MsgOK, title.data(), text.data(), noReturnBuffer, 0);
}

// Bridges DC's tProgressProcW to the ProgressFn AdbClient/PluginCore
// expect: sourceName/targetName are DC's own WCHAR* arguments to
// FsGetFileW/FsPutFileW, valid for the lifetime of that call, so they are
// captured by pointer rather than re-converted. A non-zero return from
// gProgressProcW means "abort", which is why the result is inverted
// before returning it as ProgressFn's "keep going" boolean.
ProgressFn makeProgressFn(WCHAR* sourceName, WCHAR* targetName) {
    return [sourceName, targetName](uint64_t done, uint64_t total) -> bool {
        if (gProgressProcW == nullptr) {
            return true;
        }
        int percent = computeTransferPercent(done, total);
        int abortRequested = gProgressProcW(gPluginNr, sourceName, targetName, percent);
        return abortRequested == 0;
    };
}

// The FS_STATUS_OP_* values (sdk.h) that mutate the remote filesystem --
// the operations after which a directory's cached listing (PluginCore's
// ListingCache) can no longer be trusted. Read-only operations (LIST,
// SEARCH, CALCSIZE, ...) are deliberately excluded: clearing the cache at
// the start of every one of those would defeat the cache entirely, since
// DC brackets a plain directory listing in FsStatusInfo(START, LIST) /
// FsStatusInfo(END, LIST) too.
bool isWriteOperation(int operation) {
    switch (operation) {
        case FS_STATUS_OP_PUT_SINGLE:
        case FS_STATUS_OP_PUT_MULTI:
        case FS_STATUS_OP_PUT_MULTI_THREAD:
        case FS_STATUS_OP_RENMOV_SINGLE:
        case FS_STATUS_OP_RENMOV_MULTI:
        case FS_STATUS_OP_DELETE:
        case FS_STATUS_OP_ATTRIB:
        case FS_STATUS_OP_MKDIR:
            return true;
        default:
            return false;
    }
}

} // namespace

extern "C" {

int DCPCALL FsInitW(int pluginNr, tProgressProcW progressProc, tLogProcW logProc,
                     tRequestProcW requestProc) {
    try {
        gPluginNr = pluginNr;
        gProgressProcW = progressProc;
        gLogProcW = logProc;
        gRequestProcW = requestProc;

        auto getEnv = [](const char* name) -> const char* { return std::getenv(name); };
        auto isExecutable = [](const std::string& path) -> bool {
            return ::access(path.c_str(), X_OK) == 0;
        };

        int port = adbServerPort(getEnv);
        std::string adbBinary = findAdbBinary(getEnv, isExecutable);
        if (!adbBinary.empty()) {
            startAdbServer(adbBinary);
        }

        TransportFactory factory = [port](std::string* error) -> std::unique_ptr<Transport> {
            return TcpTransport::connectTo(ADB_SERVER_HOST, port, error);
        };

        gAdbClient = std::make_unique<AdbClient>(std::move(factory));
        gPluginCore = std::make_unique<PluginCore>(*gAdbClient);

        return 0;
    } catch (...) {
        return 0; // FsInitW has no error return in this SDK; nothing else to report through.
    }
}

HANDLE DCPCALL FsFindFirstW(WCHAR* path, WIN32_FIND_DATAW* findData) {
    try {
        if (!gPluginCore) {
            return INVALID_HANDLE_VALUE;
        }

        std::string wfxDir = wideToUtf8(path);
        auto handle = std::make_unique<FindHandle>();
        std::string warning;
        std::string error;
        bool ok = gPluginCore->listDirectory(wfxDir, &handle->entries, &warning, &error);
        if (!ok) {
            return INVALID_HANDLE_VALUE;
        }

        reportWarning(warning);

        if (!advanceFindData(handle.get(), findData)) {
            return INVALID_HANDLE_VALUE; // genuinely empty (or every entry was unfittable)
        }
        return handle.release();
    } catch (...) {
        return INVALID_HANDLE_VALUE;
    }
}

BOOL DCPCALL FsFindNextW(HANDLE hdl, WIN32_FIND_DATAW* findData) {
    try {
        if (hdl == nullptr || hdl == INVALID_HANDLE_VALUE) {
            return false;
        }
        auto* handle = static_cast<FindHandle*>(hdl);
        return advanceFindData(handle, findData);
    } catch (...) {
        return false;
    }
}

int DCPCALL FsFindClose(HANDLE hdl) {
    try {
        if (hdl != nullptr && hdl != INVALID_HANDLE_VALUE) {
            delete static_cast<FindHandle*>(hdl);
        }
    } catch (...) {
        // Nothing to do -- FsFindClose has no error return, and delete on
        // an already-valid pointer should never throw in the first place.
    }
    return 0;
}

int DCPCALL FsGetFileW(WCHAR* remoteName, WCHAR* localName, int copyFlags, RemoteInfoStruct* ri) {
    try {
        (void)ri; // ri restates size/attributes DC already knows from FsFindFirstW; unused here
        if (!gPluginCore) {
            return FS_FILE_READERROR;
        }
        std::string wfxRemote = wideToUtf8(remoteName);
        std::string localPath = wideToUtf8(localName);
        std::string error;
        return gPluginCore->getFile(wfxRemote, localPath, copyFlags,
                                    makeProgressFn(remoteName, localName), &error);
    } catch (...) {
        return FS_FILE_READERROR;
    }
}

int DCPCALL FsPutFileW(WCHAR* localName, WCHAR* remoteName, int copyFlags) {
    try {
        if (!gPluginCore) {
            return FS_FILE_WRITEERROR;
        }
        std::string localPath = wideToUtf8(localName);
        std::string wfxRemote = wideToUtf8(remoteName);
        std::string error;
        return gPluginCore->putFile(localPath, wfxRemote, copyFlags,
                                    makeProgressFn(localName, remoteName), &error);
    } catch (...) {
        return FS_FILE_WRITEERROR;
    }
}

int DCPCALL FsRenMovFileW(WCHAR* oldName, WCHAR* newName, BOOL move, BOOL overWrite,
                          RemoteInfoStruct* ri) {
    try {
        (void)ri;
        if (!gPluginCore) {
            return FS_FILE_NOTSUPPORTED;
        }
        std::string wfxFrom = wideToUtf8(oldName);
        std::string wfxTo = wideToUtf8(newName);
        std::string error;
        bool ok = gPluginCore->renameOrMove(wfxFrom, wfxTo, move != 0, overWrite != 0, &error);
        // renameOrMove/PluginCore reports failures as a bool + a free-text
        // reason (cross-device move, a shell error, ...) rather than an
        // FS_FILE_* taxonomy -- there is no finer-grained code to forward
        // here, so any failure maps to FS_FILE_NOTSUPPORTED, telling DC to
        // fall back to its own get+put+delete emulation.
        return ok ? FS_FILE_OK : FS_FILE_NOTSUPPORTED;
    } catch (...) {
        return FS_FILE_NOTSUPPORTED;
    }
}

BOOL DCPCALL FsDeleteFileW(WCHAR* remoteName) {
    try {
        if (!gPluginCore) {
            return false;
        }
        std::string wfxRemote = wideToUtf8(remoteName);
        std::string error;
        return gPluginCore->deleteFile(wfxRemote, &error);
    } catch (...) {
        return false;
    }
}

BOOL DCPCALL FsRemoveDirW(WCHAR* remoteName) {
    try {
        if (!gPluginCore) {
            return false;
        }
        std::string wfxRemote = wideToUtf8(remoteName);
        std::string error;
        return gPluginCore->removeDir(wfxRemote, &error);
    } catch (...) {
        return false;
    }
}

BOOL DCPCALL FsMkDirW(WCHAR* path) {
    try {
        if (!gPluginCore) {
            return false;
        }
        std::string wfxDir = wideToUtf8(path);
        std::string error;
        return gPluginCore->makeDir(wfxDir, &error);
    } catch (...) {
        return false;
    }
}

// Non-negotiable (see task-9-brief.md and constraints.md): Double
// Commander gates copying file dates, in *both* directions, on
// Assigned(FsSetTime) or Assigned(FsSetTimeW). Without this export, DC
// silently strips caoCopyTime from every copy operation's flags -- so
// dates would be lost even when downloading, where this plugin plays no
// part in setting them. Must always be present in the export table;
// scripts/check-exports.sh checks for it by name.
BOOL DCPCALL FsSetTimeW(WCHAR* remoteName, FILETIME* creationTime, FILETIME* lastAccessTime,
                        FILETIME* lastWriteTime) {
    try {
        (void)creationTime;   // ignored per the brief: only LastWriteTime is applied
        (void)lastAccessTime; // Android's toybox touch has no separate atime knob worth using
        if (!gPluginCore || lastWriteTime == nullptr) {
            return false;
        }
        std::string wfxRemote = wideToUtf8(remoteName);
        time_t mtime = fileTimeToTime(*lastWriteTime);
        std::string error;
        return gPluginCore->setModificationTime(wfxRemote, static_cast<int64_t>(mtime), &error);
    } catch (...) {
        return false;
    }
}

void DCPCALL FsStatusInfoW(WCHAR* remoteDir, int infoStartEnd, int infoOperation) {
    try {
        if (!gPluginCore) {
            return;
        }
        if (infoStartEnd != FS_STATUS_START || !isWriteOperation(infoOperation)) {
            return;
        }

        gPluginCore->cache().clear();

        if (gLogProcW != nullptr) {
            std::string message =
                "ADB: starting a write operation on " + wideToUtf8(remoteDir) +
                " -- clearing the directory listing cache";
            std::vector<WCHAR> wideMessage = utf8ToWide(message);
            gLogProcW(gPluginNr, MSGTYPE_DETAILS, wideMessage.data());
        }
    } catch (...) {
        // FsStatusInfoW has no error return; nothing else to do.
    }
}

void DCPCALL FsGetDefRootName(char* defRootName, int maxlen) {
    try {
        if (defRootName == nullptr || maxlen <= 0) {
            return;
        }
        size_t len = std::strlen(ROOT_NAME);
        size_t copyLen = (len < static_cast<size_t>(maxlen - 1)) ? len
                                                                  : static_cast<size_t>(maxlen - 1);
        std::memcpy(defRootName, ROOT_NAME, copyLen);
        defRootName[copyLen] = '\0';
    } catch (...) {
        if (defRootName != nullptr && maxlen > 0) {
            defRootName[0] = '\0';
        }
    }
}

BOOL DCPCALL FsDisconnectW(WCHAR* disconnectRoot) {
    try {
        (void)disconnectRoot; // one shared cache for every device; nothing to key this on
        if (!gPluginCore) {
            return true;
        }
        gPluginCore->cache().clear();
        return true;
    } catch (...) {
        return false;
    }
}

} // extern "C"
