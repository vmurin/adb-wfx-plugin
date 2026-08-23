// fsplugin.cpp -- the extern "C" export shim, and the ONLY translation
// unit besides tests/*.cpp in this project.
//
// This file owns exactly: process-global state (the callbacks DC hands
// FsInitW and the AdbClient/PluginCore pair) and UTF-16<->UTF-8
// conversion at the SDK boundary (utils.hpp). The find-handle lifecycle,
// progress/warning/error callback bridging, and the write-operation
// classification for FsStatusInfoW are all pure functions hoisted into
// fsplugin_impl.hpp (FindHandle, advanceFindData, reportWarning,
// reportError, makeProgressFn, isWriteOperation, isUnsetFileTime) so they
// are directly testable; this file only supplies its globals as their
// arguments. Every actual decision -- device naming, caching, the
// download write scheme (direct, or temp-then-rename when
// overwriting), mtime preservation, which shell commands the mutating
// operations run -- lives in PluginCore
// (fsplugin_impl.hpp). If a change here starts to look like a
// policy decision rather than a translation, it belongs there instead.
//
// No C++ exception may ever cross this file's extern "C" boundary -- an
// escaping exception terminates Double Commander, not just the plugin.
// Every export body is therefore `try { ... } catch (...) { return
// <the appropriate error value>; }`.
//
// Threading note: Double Commander can call a WFX plugin from more than
// one thread (its background-transfer modes). Making this file's global
// state thread-safe is out of scope for this task; the globals below are
// kept to exactly the ones FsInitW needs to hand out, and nothing here
// does anything to make concurrent use *more* broken than the single
// AdbClient/PluginCore pair already implies.
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

// compile_mac.sh/compile_mac_universal.sh build with -fvisibility=hidden,
// so every symbol is hidden from the shared library's export table by
// default -- this file's own helper functions and globals included. Each
// of the exports Double Commander actually calls is marked with this
// attribute to opt back in; scripts/check-exports.sh and
// tests/test_exports.cpp both verify the result names exactly the 14
// exports Double Commander requires, nothing else (previously, an
// un-hidden libc++ internal symbol showed up alongside them in
// `nm -gU`).
#if defined(__GNUC__) || defined(__clang__)
#define WFX_EXPORT __attribute__((visibility("default")))
#else
#define WFX_EXPORT
#endif

namespace {

// Host and port fsplugin.cpp's TransportFactory connects to. The port
// itself is resolved by adbServerPort() (ANDROID_ADB_SERVER_PORT,
// falling back to ADB_DEFAULT_PORT); only the loopback host is fixed here.
constexpr const char* ADB_SERVER_HOST = "127.0.0.1";

// The value FsGetDefRootName copies out, and the WFX root path's first
// component everywhere a device serial or model name isn't in play yet.
constexpr const char* ROOT_NAME = "ADB";

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

} // namespace

extern "C" {

WFX_EXPORT int DCPCALL FsInitW(int pluginNr, tProgressProcW progressProc, tLogProcW logProc,
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

        // Order matters on a second FsInitW: PluginCore holds an
        // AdbClient& , so replacing the client first would leave the old
        // core holding a dangling reference for the length of one
        // statement. Tear the core down before the client it points at.
        gPluginCore.reset();
        gAdbClient = std::make_unique<AdbClient>(std::move(factory));
        gPluginCore = std::make_unique<PluginCore>(*gAdbClient);

        return 0;
    } catch (...) {
        return 0; // FsInitW has no error return in this SDK; nothing else to report through.
    }
}

WFX_EXPORT HANDLE DCPCALL FsFindFirstW(WCHAR* path, WIN32_FIND_DATAW* findData) {
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
            reportError(gLogProcW, gPluginNr, error);
            return INVALID_HANDLE_VALUE;
        }

        reportWarning(gRequestProcW, gPluginNr, warning);

        if (!advanceFindData(handle.get(), findData)) {
            return INVALID_HANDLE_VALUE; // genuinely empty (or every entry was unfittable)
        }
        return handle.release();
    } catch (...) {
        return INVALID_HANDLE_VALUE;
    }
}

WFX_EXPORT BOOL DCPCALL FsFindNextW(HANDLE hdl, WIN32_FIND_DATAW* findData) {
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

WFX_EXPORT int DCPCALL FsFindClose(HANDLE hdl) {
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

WFX_EXPORT int DCPCALL FsGetFileW(WCHAR* remoteName, WCHAR* localName, int copyFlags, RemoteInfoStruct* ri) {
    try {
        (void)ri; // ri restates size/attributes DC already knows from FsFindFirstW; unused here
        if (!gPluginCore) {
            return FS_FILE_READERROR;
        }
        std::string wfxRemote = wideToUtf8(remoteName);
        std::string localPath = wideToUtf8(localName);
        std::string error;
        int result = gPluginCore->getFile(
            wfxRemote, localPath, copyFlags,
            makeProgressFn(gProgressProcW, gPluginNr, remoteName, localName), &error);
        if (result != FS_FILE_OK) {
            reportError(gLogProcW, gPluginNr, error); // empty for USERABORT/EXISTS/NOTFOUND: no-op
        }
        return result;
    } catch (...) {
        return FS_FILE_READERROR;
    }
}

WFX_EXPORT int DCPCALL FsPutFileW(WCHAR* localName, WCHAR* remoteName, int copyFlags) {
    try {
        if (!gPluginCore) {
            return FS_FILE_WRITEERROR;
        }
        std::string localPath = wideToUtf8(localName);
        std::string wfxRemote = wideToUtf8(remoteName);
        std::string error;
        int result = gPluginCore->putFile(
            localPath, wfxRemote, copyFlags,
            makeProgressFn(gProgressProcW, gPluginNr, localName, remoteName), &error);
        if (result != FS_FILE_OK) {
            reportError(gLogProcW, gPluginNr, error); // empty for USERABORT/EXISTS: no-op
        }
        return result;
    } catch (...) {
        return FS_FILE_WRITEERROR;
    }
}

WFX_EXPORT int DCPCALL FsRenMovFileW(WCHAR* oldName, WCHAR* newName, BOOL move, BOOL overWrite,
                          RemoteInfoStruct* ri) {
    try {
        (void)ri;
        if (!gPluginCore) {
            return FS_FILE_WRITEERROR;
        }
        std::string wfxFrom = wideToUtf8(oldName);
        std::string wfxTo = wideToUtf8(newName);
        std::string error;
        bool crossDevice = false;
        bool targetExists = false;
        bool ok = gPluginCore->renameOrMove(wfxFrom, wfxTo, move != 0, overWrite != 0, &error,
                                            &crossDevice, &targetExists);
        if (ok) {
            return FS_FILE_OK;
        }
        reportError(gLogProcW, gPluginNr, error);
        // A refused overwrite maps to FS_FILE_EXISTS -- the same code
        // getFile/putFile already return for the identical situation
        // (wfxplugin.h defines it for exactly "target exists and
        // OverWrite was false") -- checked ahead of crossDevice since the
        // two are mutually exclusive by construction (PluginCore checks
        // cross-device before ever reaching the overwrite check). A
        // cross-device rejection maps to FS_FILE_NOTSUPPORTED, telling DC
        // to fall back to its own get+put+delete emulation -- the one
        // remaining failure mode that fallback can actually fix. Every
        // other failure (a shell error, a dropped transport) maps to
        // FS_FILE_WRITEERROR instead: retrying the identical operation as
        // a download+upload+delete would not fix a genuine error, and for
        // a multi-gigabyte file wastes minutes attributing the failure to
        // the wrong step.
        if (targetExists) {
            return FS_FILE_EXISTS;
        }
        return crossDevice ? FS_FILE_NOTSUPPORTED : FS_FILE_WRITEERROR;
    } catch (...) {
        return FS_FILE_WRITEERROR;
    }
}

WFX_EXPORT BOOL DCPCALL FsDeleteFileW(WCHAR* remoteName) {
    try {
        if (!gPluginCore) {
            return false;
        }
        std::string wfxRemote = wideToUtf8(remoteName);
        std::string error;
        bool ok = gPluginCore->deleteFile(wfxRemote, &error);
        if (!ok) {
            reportError(gLogProcW, gPluginNr, error);
        }
        return ok;
    } catch (...) {
        return false;
    }
}

WFX_EXPORT BOOL DCPCALL FsRemoveDirW(WCHAR* remoteName) {
    try {
        if (!gPluginCore) {
            return false;
        }
        std::string wfxRemote = wideToUtf8(remoteName);
        std::string error;
        bool ok = gPluginCore->removeDir(wfxRemote, &error);
        if (!ok) {
            reportError(gLogProcW, gPluginNr, error);
        }
        return ok;
    } catch (...) {
        return false;
    }
}

WFX_EXPORT BOOL DCPCALL FsMkDirW(WCHAR* path) {
    try {
        if (!gPluginCore) {
            return false;
        }
        std::string wfxDir = wideToUtf8(path);
        std::string error;
        bool ok = gPluginCore->makeDir(wfxDir, &error);
        if (!ok) {
            reportError(gLogProcW, gPluginNr, error);
        }
        return ok;
    } catch (...) {
        return false;
    }
}

// Non-negotiable: Double
// Commander gates copying file dates, in *both* directions, on
// Assigned(FsSetTime) or Assigned(FsSetTimeW). Without this export, DC
// silently strips caoCopyTime from every copy operation's flags -- so
// dates would be lost even when downloading, where this plugin plays no
// part in setting them. Must always be present in the export table;
// scripts/check-exports.sh checks for it by name.
WFX_EXPORT BOOL DCPCALL FsSetTimeW(WCHAR* remoteName, FILETIME* creationTime, FILETIME* lastAccessTime,
                        FILETIME* lastWriteTime) {
    try {
        (void)creationTime;   // ignored: only LastWriteTime is applied
        (void)lastAccessTime; // Android's toybox touch has no separate atime knob worth using
        if (!gPluginCore) {
            return false;
        }
        if (isUnsetFileTime(lastWriteTime)) {
            return true; // nothing to set is not a failure
        }
        std::string wfxRemote = wideToUtf8(remoteName);
        time_t mtime = fileTimeToTime(*lastWriteTime);
        std::string error;
        bool ok = gPluginCore->setModificationTime(wfxRemote, static_cast<int64_t>(mtime), &error);
        if (!ok) {
            reportError(gLogProcW, gPluginNr, error);
        }
        return ok;
    } catch (...) {
        return false;
    }
}

WFX_EXPORT void DCPCALL FsStatusInfoW(WCHAR* remoteDir, int infoStartEnd, int infoOperation) {
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

WFX_EXPORT void DCPCALL FsGetDefRootName(char* defRootName, int maxlen) {
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

WFX_EXPORT BOOL DCPCALL FsDisconnectW(WCHAR* disconnectRoot) {
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
