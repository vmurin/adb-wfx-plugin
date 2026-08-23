// PluginCore: the WFX logic layer. Every decision the plugin makes --
// device naming, caching, the download-to-temp-then-rename dance, mtime
// preservation in both directions, and the shell commands used for the
// mutating operations the sync protocol doesn't cover -- lives here, so
// that fsplugin.cpp (Task 9) can be a mechanical extern "C" shim over it.
// Works entirely in UTF-8 std::string; UTF-16 conversion is the shim's
// job, not this one's. See docs/plan-adb-wfx.md (Task 8) and
// constraints.md for the behavioural contract this was built against.
#ifndef ADB_WFX_FSPLUGIN_IMPL_HPP
#define ADB_WFX_FSPLUGIN_IMPL_HPP

#include "adbclient.hpp"
#include "adbproto.hpp"
#include "adbutils.hpp"
#include "sdk.h"
#include "utils.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// One directory entry, UTF-8. Deliberately decoupled from DirEntry (the
// ADB wire type): FindResult is what the SDK-facing shim renders into a
// WIN32_FIND_DATAW, and shouldn't need to know anything about the sync
// protocol.
struct FindResult {
    std::string name;
    uint64_t size = 0;
    int64_t mtime = 0;
    uint32_t unixMode = 0;
    bool isDir = false;
};

// ---------------------------------------------------------------------
// fsplugin.cpp (Task 9) helpers.
//
// These serve the extern "C" shim, not PluginCore -- but constraints.md
// #4 (header-only modules: only fsplugin.cpp and tests/*.cpp are
// translation units) means they have to live in a header to be testable
// directly, without dlopen'ing the built plugin. They live here, next to
// FindResult, rather than in a header of their own, because YAGNI: one
// small set of free functions doesn't earn a third header.
//
// The ones that would otherwise read fsplugin.cpp's globals (the DC
// callback pointers, the plugin number) take them as explicit parameters
// instead, purely so they can be driven from a test with a fake callback
// -- fsplugin.cpp itself just passes its globals at each call site.
// ---------------------------------------------------------------------

constexpr int TRANSFER_PERCENT_MIN = 0;
constexpr int TRANSFER_PERCENT_MAX = 100;

// done*100/total, clamped to 0..100. Computed entirely in uint64_t --
// done*100 alone can reach roughly 2^64/100, far past any real transfer
// size -- so a multi-gigabyte done/total pair cannot overflow before the
// division the way it would in a 32-bit intermediate. total == 0 (nothing
// to transfer, or the total is genuinely unknown) yields 0 rather than
// dividing by zero.
inline int computeTransferPercent(uint64_t done, uint64_t total) {
    if (total == 0) {
        return TRANSFER_PERCENT_MIN;
    }
    uint64_t percent = (done * static_cast<uint64_t>(TRANSFER_PERCENT_MAX)) / total;
    if (percent > static_cast<uint64_t>(TRANSFER_PERCENT_MAX)) {
        return TRANSFER_PERCENT_MAX;
    }
    return static_cast<int>(percent);
}

// Fills findData from entry, the shape WIN32_FIND_DATAW takes at the SDK
// boundary. Every entry gets FILE_ATTRIBUTE_UNIX_MODE with the POSIX mode
// packed into dwReserved0 (so Double Commander can show real
// permissions); directories additionally get FILE_ATTRIBUTE_DIRECTORY.
// Creation and last-access time are both set to the same value as the
// last-write time -- the sync protocol carries only one timestamp per
// entry, so there is nothing else to put there.
//
// Returns false, leaving *findData zeroed, when entry.name does not fit
// MAX_PATH UTF-16 units: the caller must skip the entry rather than use a
// truncated name that could refer to something else entirely.
inline bool fillFindData(const FindResult& entry, WIN32_FIND_DATAW* findData) {
    *findData = WIN32_FIND_DATAW{};

    if (!utf8ToWideBuf(entry.name, findData->cFileName, MAX_PATH)) {
        *findData = WIN32_FIND_DATAW{};
        return false;
    }

    findData->dwFileAttributes = static_cast<DWORD>(FILE_ATTRIBUTE_UNIX_MODE);
    if (entry.isDir) {
        findData->dwFileAttributes |= static_cast<DWORD>(FILE_ATTRIBUTE_DIRECTORY);
    }
    findData->dwReserved0 = static_cast<DWORD>(entry.unixMode);

    findData->nFileSizeLow = static_cast<DWORD>(entry.size & 0xFFFFFFFFULL);
    findData->nFileSizeHigh = static_cast<DWORD>(entry.size >> 32);

    FILETIME writeTime = timeToFileTime(static_cast<time_t>(entry.mtime));
    findData->ftLastWriteTime = writeTime;
    findData->ftCreationTime = writeTime;
    findData->ftLastAccessTime = writeTime;

    return true;
}

// One FsFindFirstW/FsFindNextW/FsFindClose cycle's state: the full
// listing (PluginCore::listDirectory has no notion of paging) plus how
// far FsFindNextW has consumed it. fsplugin.cpp hands a FindHandle* to
// Double Commander as an opaque HANDLE.
struct FindHandle {
    std::vector<FindResult> entries;
    size_t index = 0;
};

// Fills findData from the next entry in handle's listing that actually
// fits a WIN32_FIND_DATAW (fillFindData above skips one whose name
// doesn't fit MAX_PATH rather than truncate it), advancing handle->index
// past every entry it looks at -- including skipped ones, so a name that
// doesn't fit can never be looked at twice. Returns false once the
// listing is exhausted. Shared by FsFindFirstW and FsFindNextW so this
// skip-and-advance logic exists in exactly one place.
inline bool advanceFindData(FindHandle* handle, WIN32_FIND_DATAW* findData) {
    while (handle->index < handle->entries.size()) {
        const FindResult& entry = handle->entries[handle->index];
        ++handle->index;
        if (fillFindData(entry, findData)) {
            return true;
        }
    }
    return false;
}

// Raises warning (if non-empty) as an RT_MsgOK dialog via requestProc --
// used for listDirectory's *warning output (e.g. "device unauthorized",
// see PluginCore::listDirectory above), which must reach the user even
// when the rest of the listing came back fine. A null requestProc (DC
// didn't supply one) or an empty warning are both silent no-ops.
inline void reportWarning(tRequestProcW requestProc, int pluginNr, const std::string& warning) {
    if (warning.empty() || requestProc == nullptr) {
        return;
    }
    std::vector<WCHAR> title = utf8ToWide("ADB");
    std::vector<WCHAR> text = utf8ToWide(warning);
    WCHAR noReturnBuffer[1] = {0};
    requestProc(pluginNr, RT_MsgOK, title.data(), text.data(), noReturnBuffer, 0);
}

// Raises message (if non-empty) via logProc as MSGTYPE_IMPORTANTERROR --
// the shim's only channel for the free-text failure reasons PluginCore
// produces (a cross-device move, a device's shell stderr, a transport
// error, ...). Without this, every one of those reasons went nowhere and
// the user saw only Double Commander's generic "cannot copy/delete/
// rename" (see task-9 review round 1). A null logProc or an empty
// message are both silent no-ops.
inline void reportError(tLogProcW logProc, int pluginNr, const std::string& message) {
    if (message.empty() || logProc == nullptr) {
        return;
    }
    std::vector<WCHAR> wideMessage = utf8ToWide(message);
    logProc(pluginNr, MSGTYPE_IMPORTANTERROR, wideMessage.data());
}

// Bridges DC's tProgressProcW to the ProgressFn AdbClient/PluginCore
// expect: sourceName/targetName are DC's own WCHAR* arguments to
// FsGetFileW/FsPutFileW, valid for the lifetime of that call, so they are
// captured by pointer rather than re-converted. A non-zero return from
// progressProc means "abort", which is why the result is inverted before
// returning it as ProgressFn's "keep going" boolean. A null progressProc
// (DC didn't supply one) always means "keep going".
inline ProgressFn makeProgressFn(tProgressProcW progressProc, int pluginNr, WCHAR* sourceName,
                                 WCHAR* targetName) {
    return [progressProc, pluginNr, sourceName, targetName](uint64_t done, uint64_t total) -> bool {
        if (progressProc == nullptr) {
            return true;
        }
        int percent = computeTransferPercent(done, total);
        int abortRequested = progressProc(pluginNr, sourceName, targetName, percent);
        return abortRequested == 0;
    };
}

// The FS_STATUS_OP_* values (sdk.h) that mutate the remote filesystem --
// the operations after which a directory's cached listing (PluginCore's
// ListingCache) can no longer be trusted. Read-only operations (LIST,
// SEARCH, CALCSIZE, ...) are deliberately excluded: clearing the cache at
// the start of every one of those would defeat the cache entirely, since
// DC brackets a plain directory listing in FsStatusInfo(START, LIST) /
// FsStatusInfo(END, LIST) too. tests/test_fsplugin_helpers.cpp enumerates
// every FS_STATUS_OP_* constant against this function, so a future
// mutating operation added to the SDK's vocabulary without a case here
// fails loudly instead of silently serving a stale listing.
inline bool isWriteOperation(int operation) {
    switch (operation) {
        case FS_STATUS_OP_PUT_SINGLE:
        case FS_STATUS_OP_PUT_MULTI:
        case FS_STATUS_OP_PUT_MULTI_THREAD:
        case FS_STATUS_OP_RENMOV_SINGLE:
        case FS_STATUS_OP_RENMOV_MULTI:
        case FS_STATUS_OP_DELETE:
        case FS_STATUS_OP_ATTRIB:
        case FS_STATUS_OP_MKDIR:
        case FS_STATUS_OP_SYNC_PUT:
        case FS_STATUS_OP_SYNC_DELETE:
            return true;
        default:
            return false;
    }
}

// FsSetTimeW's shim-level guard: a zero-filled FILETIME (both fields 0)
// means "not provided" in this SDK -- utils.hpp's fileTimeToTime treats
// it the same way for the reverse direction ("a zero FILETIME means
// unknown") -- not epoch 0. Without this check, fileTimeToTime(*ft)
// returns time_t 0 indistinguishably from a genuine Unix-epoch mtime, and
// FsSetTimeW would stamp the file 1970-01-01 whenever LastWriteTime comes
// in null or zeroed: silently wrong dates from the very export that
// exists to protect them (see task-9 review round 1). A null pointer
// counts as unset too, so callers don't need a separate null check.
inline bool isUnsetFileTime(const FILETIME* ft) {
    return ft == nullptr || (ft->dwLowDateTime == 0 && ft->dwHighDateTime == 0);
}

namespace plugincore_detail {

// Separates a device's model from its serial in the display name shown at
// the WFX root: "<model> (<serial>)". serialFromDisplayName below must be
// the exact inverse of pasting this in.
constexpr const char* DISPLAY_NAME_SERIAL_OPEN = " (";
constexpr char DISPLAY_NAME_SERIAL_CLOSE = ')';

// "YYYYMMDDhhmm.ss\0" -- the buffer for touch -t's fallback argument.
constexpr size_t TOUCH_T_TIMESTAMP_BUFFER_SIZE = 16;

// A local file gets read/write owner-only permissions while it's still a
// temp download -- rename() carries the final permissions decision (there
// is none here; this plugin doesn't attempt to mirror POSIX file modes
// onto the local macOS filesystem for downloads).
constexpr mode_t TEMP_DOWNLOAD_FILE_MODE = S_IRUSR | S_IWUSR;

// Trims a run of trailing '\n'/'\r' bytes. Android's shell: gives no exit
// status on this transport, so "was there any output at all" is how
// rm/mv/mkdir/touch's success is detected -- and their success output is a
// trailing newline, not truly empty, so this has to run before the
// emptiness check.
inline std::string trimTrailingNewlines(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && (s[end - 1] == '\n' || s[end - 1] == '\r')) {
        --end;
    }
    return s.substr(0, end);
}

// The WFX directory that contains wfxPath, e.g.
// "/SERIAL/sdcard/DCIM/a.jpg" -> "/SERIAL/sdcard/DCIM". Falls back to "/"
// for a path with no further separator (a bare device root's child).
inline std::string parentWfxDir(const std::string& wfxPath) {
    size_t pos = wfxPath.find_last_of('/');
    if (pos == std::string::npos || pos == 0) {
        return "/";
    }
    return wfxPath.substr(0, pos);
}

inline bool localFileExists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

// getFile downloads here first and rename()s it into place next to the
// real target, so an interrupted transfer never leaves a truncated file
// at the real name. The pid suffix is cheap insurance against two plugin
// instances racing on the same target.
inline std::string tempDownloadPath(const std::string& localPath) {
    return localPath + ".adbwfx.tmp." + std::to_string(::getpid());
}

// Formats mtime (seconds since the Unix epoch) as touch -t's
// [[CC]YY]MMDDhhmm[.ss] argument, in UTC via gmtime_r -- deliberately NOT
// localtime_r. This is only reached as setModificationTime's fallback for
// when Android's toybox touch rejects "-d @<epoch>"; converting through
// the *local* timezone here would silently shift every uploaded or
// renamed date by the machine's UTC offset, which is exactly the class of
// correctness bug this whole project exists to eliminate. The other half
// of that agreement lives at the call site: touch -t reads its argument
// in the *device's* local time, so setModificationTime prefixes the
// command with TZ=UTC. Change one and you must change the other.
//
// Returns false (leaving *out untouched) when epochSeconds is out of
// gmtime_r's representable range, rather than silently building a shell
// command around whatever garbage a zero-initialized struct tm would
// format as (e.g. "190001000000.00").
inline bool formatTouchTArg(int64_t epochSeconds, std::string* out) {
    time_t t = static_cast<time_t>(epochSeconds);
    struct tm utc {};
    if (::gmtime_r(&t, &utc) == nullptr) {
        return false;
    }
    char buf[TOUCH_T_TIMESTAMP_BUFFER_SIZE];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d.%02d", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    *out = buf;
    return true;
}

} // namespace plugincore_detail

class PluginCore {
public:
    explicit PluginCore(AdbClient& client) : client_(client) {}

    // Same, with the listing cache's clock and TTL injected -- for tests
    // that need to prove expiry without sleeping through it.
    PluginCore(AdbClient& client, ClockFn clock, int64_t cacheTtlSeconds)
        : client_(client), cache_(std::move(clock), cacheTtlSeconds) {}

    // Returns entries for a WFX directory path. Root ("/") returns one
    // entry per usable device; every other path lists the device's
    // on-device directory via the sync protocol, through the cache.
    bool listDirectory(const std::string& wfxDir, std::vector<FindResult>* out,
                       std::string* warning, std::string* error) {
        out->clear();
        if (warning != nullptr) {
            warning->clear();
        }
        if (error != nullptr) {
            error->clear();
        }

        RemotePath rp = parseWfxPath(wfxDir);
        if (rp.isRoot) {
            return listRootDevices(out, warning, error);
        }

        std::vector<DirEntry> cached;
        if (cache_.get(wfxDir, &cached)) {
            appendEntries(cached, out);
            return true;
        }

        // adbd's do_list answers a failed opendir() with a plain DONE and
        // zero DENTs -- it never sends FAIL. A path that does not exist,
        // a path that is actually a file, and a directory that is
        // genuinely empty are therefore indistinguishable at the LIST
        // level: all three come back as a successful empty listing, get
        // cached as empty, and leave Double Commander showing a blank
        // panel with no explanation for what is usually a typo. STAT
        // first and name the problem.
        //
        // The one case this still cannot separate from "empty" is a
        // directory that exists but cannot be opened (/data, /data/data
        // on an unrooted phone): the sync protocol offers nothing to
        // distinguish it by short of guessing at mode bits and a uid the
        // client does not know.
        if (!checkIsDirectory(rp.serial, rp.path, error)) {
            return false;
        }

        std::vector<DirEntry> entries;
        AdbError err = client_.syncList(rp.serial, rp.path, &entries);
        if (!err.ok) {
            if (error != nullptr) {
                *error = err.message;
            }
            return false;
        }

        resolveSymlinkTargets(rp.serial, rp.path, &entries);

        cache_.put(wfxDir, entries);
        appendEntries(entries, out);
        return true;
    }

    // "<model> (<serial>)" when a model is known, else the bare serial.
    static std::string displayNameForDevice(const DeviceInfo& d) {
        if (!d.model.empty()) {
            return d.model + plugincore_detail::DISPLAY_NAME_SERIAL_OPEN + d.serial +
                   plugincore_detail::DISPLAY_NAME_SERIAL_CLOSE;
        }
        return d.serial;
    }

    // The exact inverse of displayNameForDevice: pulls the serial back out
    // of "<model> (<serial>)", or returns the whole string when it never
    // had a "(<serial>)" suffix (the no-model case).
    static std::string serialFromDisplayName(const std::string& displayName) {
        if (displayName.empty() || displayName.back() != plugincore_detail::DISPLAY_NAME_SERIAL_CLOSE) {
            return displayName;
        }
        size_t openPos = displayName.rfind(plugincore_detail::DISPLAY_NAME_SERIAL_OPEN);
        if (openPos == std::string::npos) {
            return displayName;
        }
        size_t start = openPos + std::strlen(plugincore_detail::DISPLAY_NAME_SERIAL_OPEN);
        size_t end = displayName.size() - 1; // exclude the trailing ')'
        if (end <= start) {
            return displayName;
        }
        return displayName.substr(start, end - start);
    }

    int getFile(const std::string& wfxRemote, const std::string& localPath, int copyFlags,
                const ProgressFn& progress, std::string* error) {
        if (error != nullptr) {
            error->clear();
        }
        if (copyFlags & FS_COPYFLAGS_RESUME) {
            return FS_FILE_NOTSUPPORTED;
        }
        RemotePath rp = parseWfxPath(wfxRemote);

        DirEntry remoteInfo;
        bool exists = false;
        AdbError statErr = client_.syncStat(rp.serial, rp.path, &remoteInfo, &exists);
        if (!statErr.ok) {
            if (error != nullptr) {
                *error = statErr.message;
            }
            return FS_FILE_READERROR;
        }
        if (!exists) {
            return FS_FILE_NOTFOUND;
        }

        // The STAT above is adbd's lstat, so for a symlink it describes
        // the LINK -- a 21-byte size and the link's own mtime -- while
        // syncRecv below downloads the TARGET. Left uncorrected that
        // makes the progress percentage meaningless and stamps the
        // downloaded file with the link's date, which is exactly the
        // class of silently-wrong mtime this project exists to
        // eliminate. Re-stat through the link and use the target's
        // numbers. Best-effort: a dangling link keeps the lstat values
        // and syncRecv reports the real error.
        if (remoteInfo.isSymlink()) {
            DirEntry target;
            bool targetExists = false;
            AdbError targetErr =
                client_.syncStat(rp.serial, throughSymlink(rp.path), &target, &targetExists);
            if (targetErr.ok && targetExists) {
                remoteInfo = target;
            }
        }

        if (!(copyFlags & FS_COPYFLAGS_OVERWRITE) && plugincore_detail::localFileExists(localPath)) {
            return FS_FILE_EXISTS;
        }

        std::string tempPath = plugincore_detail::tempDownloadPath(localPath);
        int fd = ::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                         plugincore_detail::TEMP_DOWNLOAD_FILE_MODE);
        if (fd < 0) {
            if (error != nullptr) {
                *error = std::string("failed to create temporary file: ") + std::strerror(errno);
            }
            return FS_FILE_WRITEERROR;
        }

        AdbError recvErr = client_.syncRecv(rp.serial, rp.path, fd, remoteInfo.size, progress);
        ::close(fd);

        if (!recvErr.ok) {
            ::unlink(tempPath.c_str());
            if (recvErr.message == ADB_CANCELLED) {
                return FS_FILE_USERABORT;
            }
            if (error != nullptr) {
                *error = recvErr.message;
            }
            return FS_FILE_READERROR;
        }

        if (::rename(tempPath.c_str(), localPath.c_str()) != 0) {
            std::string renameError = std::strerror(errno);
            ::unlink(tempPath.c_str());
            if (error != nullptr) {
                *error = "failed to move downloaded file into place: " + renameError;
            }
            return FS_FILE_WRITEERROR;
        }

        // The headline requirement of the entire project: the local
        // file's mtime comes from the remote, not from the moment the
        // download finished. Set on the final path, after the rename, so
        // rename() (which does not touch mtime on this filesystem) can
        // never race it away.
        struct timespec times[2];
        times[0].tv_sec = 0;
        times[0].tv_nsec = UTIME_OMIT; // leave atime alone
        times[1].tv_sec = static_cast<time_t>(remoteInfo.mtime);
        times[1].tv_nsec = 0;
        if (::utimensat(AT_FDCWD, localPath.c_str(), times, 0) != 0) {
            // A file left behind with a silently-wrong mtime is exactly
            // the class of defect this project exists to eliminate, and
            // worse than a failed getFile leaving nothing behind: remove
            // it so a failed copy cannot masquerade as a complete one.
            std::string utimeError = std::strerror(errno);
            ::unlink(localPath.c_str());
            if (error != nullptr) {
                *error = "downloaded but failed to set local modification time, file removed: " +
                         utimeError;
            }
            return FS_FILE_WRITEERROR;
        }

        if (copyFlags & FS_COPYFLAGS_MOVE) {
            std::string deleteError; // best-effort: the download already succeeded
            deleteFile(wfxRemote, &deleteError);
        }

        return FS_FILE_OK;
    }

    int putFile(const std::string& localPath, const std::string& wfxRemote, int copyFlags,
                const ProgressFn& progress, std::string* error) {
        if (error != nullptr) {
            error->clear();
        }
        if (copyFlags & FS_COPYFLAGS_RESUME) {
            return FS_FILE_NOTSUPPORTED;
        }
        RemotePath rp = parseWfxPath(wfxRemote);

        struct stat localStat;
        if (::stat(localPath.c_str(), &localStat) != 0) {
            if (error != nullptr) {
                *error = std::string("failed to read local file: ") + std::strerror(errno);
            }
            return FS_FILE_READERROR;
        }

        if (!(copyFlags & FS_COPYFLAGS_OVERWRITE)) {
            DirEntry remoteInfo;
            bool exists = false;
            AdbError statErr = client_.syncStat(rp.serial, rp.path, &remoteInfo, &exists);
            if (!statErr.ok) {
                if (error != nullptr) {
                    *error = statErr.message;
                }
                return FS_FILE_WRITEERROR;
            }
            if (exists) {
                return FS_FILE_EXISTS;
            }
        }

        int fd = ::open(localPath.c_str(), O_RDONLY);
        if (fd < 0) {
            if (error != nullptr) {
                *error = std::string("failed to open local file: ") + std::strerror(errno);
            }
            return FS_FILE_READERROR;
        }

        // The other half of the headline requirement: the local file's
        // real mtime is what gets sent -- never time(nullptr).
        AdbError sendErr = client_.syncSend(rp.serial, fd, static_cast<uint64_t>(localStat.st_size),
                                            rp.path, static_cast<uint32_t>(localStat.st_mode),
                                            static_cast<int64_t>(localStat.st_mtime), progress);
        ::close(fd);

        if (!sendErr.ok) {
            if (sendErr.message == ADB_CANCELLED) {
                // A cancelled send may already have written a partial
                // file remotely (syncSend only cancels between DATA
                // chunks, never mid-chunk) -- drop any cached listing of
                // its directory so it isn't hidden behind stale state.
                invalidatePathAndParent(wfxRemote);
                return FS_FILE_USERABORT;
            }
            if (error != nullptr) {
                *error = sendErr.message;
            }
            return FS_FILE_WRITEERROR;
        }

        invalidatePathAndParent(wfxRemote);

        if (copyFlags & FS_COPYFLAGS_MOVE) {
            ::unlink(localPath.c_str());
        }

        return FS_FILE_OK;
    }

    bool deleteFile(const std::string& wfxRemote, std::string* error) {
        RemotePath rp = parseWfxPath(wfxRemote);
        return runMutatingShellCommand(rp, "rm -f " + shellQuote(rp.path), wfxRemote, error);
    }

    bool removeDir(const std::string& wfxRemote, std::string* error) {
        RemotePath rp = parseWfxPath(wfxRemote);
        return runMutatingShellCommand(rp, "rm -rf " + shellQuote(rp.path), wfxRemote, error);
    }

    bool makeDir(const std::string& wfxRemote, std::string* error) {
        RemotePath rp = parseWfxPath(wfxRemote);
        return runMutatingShellCommand(rp, "mkdir -p " + shellQuote(rp.path), wfxRemote, error);
    }

    // crossDevice and targetExists, if non-null, are both cleared at the
    // start and each set to true only for its own specific failure --
    // "wfxFrom and wfxTo name different devices" for the former, "the
    // target exists and overwrite is false" for the latter -- so
    // fsplugin.cpp can map each to the SDK code Double Commander actually
    // needs (FS_FILE_NOTSUPPORTED, the one case its own copy+delete
    // fallback can fix; FS_FILE_EXISTS, matching the code getFile/putFile
    // already return for the identical situation) without string-matching
    // *error. Every other failure leaves both false: retrying via that
    // fallback would not fix a genuine error (permission denied, a
    // dropped transport), and for a multi-gigabyte file wastes minutes
    // attributing the failure to the wrong step.
    bool renameOrMove(const std::string& wfxFrom, const std::string& wfxTo, bool move,
                      bool overwrite, std::string* error, bool* crossDevice = nullptr,
                      bool* targetExists = nullptr) {
        // A same-directory rename and a cross-directory move both land on
        // the same "mv" against the device's single filesystem tree --
        // the distinction only matters to Double Commander's UI, never to
        // the shell command issued here.
        (void)move;

        if (error != nullptr) {
            error->clear();
        }
        if (crossDevice != nullptr) {
            *crossDevice = false;
        }
        if (targetExists != nullptr) {
            *targetExists = false;
        }
        RemotePath rpFrom = parseWfxPath(wfxFrom);
        RemotePath rpTo = parseWfxPath(wfxTo);

        if (rpFrom.serial != rpTo.serial) {
            // The sync/shell protocol is scoped to one host:transport:
            // device at a time; there is no remote-to-remote copy
            // primitive to build a cross-device move on top of (that
            // would need a download-then-upload, which is out of scope
            // here). Reject explicitly rather than silently running mv
            // on the wrong device and reporting success.
            if (error != nullptr) {
                *error = "cannot move between devices";
            }
            if (crossDevice != nullptr) {
                *crossDevice = true;
            }
            return false;
        }

        if (!overwrite) {
            // "mv -n" against an existing target exits 0 and prints
            // nothing on this shell -- indistinguishable, via
            // runMutatingShellCommand-style "empty output means success"
            // logic, from a rename that actually happened (see task-9
            // review round 1). This transport gives no exit status to
            // lean on, so the only reliable way to honor "don't
            // overwrite" is to check first and never send -n at all.
            DirEntry targetInfo;
            bool targetAlreadyExists = false;
            AdbError statErr =
                client_.syncStat(rpTo.serial, rpTo.path, &targetInfo, &targetAlreadyExists);
            if (!statErr.ok) {
                if (error != nullptr) {
                    *error = statErr.message;
                }
                return false;
            }
            if (targetAlreadyExists) {
                if (error != nullptr) {
                    *error = "target already exists";
                }
                if (targetExists != nullptr) {
                    *targetExists = true;
                }
                return false;
            }
        }

        std::string command = "mv " + shellQuote(rpFrom.path) + " " + shellQuote(rpTo.path);

        std::string output;
        AdbError err = client_.shellCommand(rpFrom.serial, command, &output);
        if (!err.ok) {
            if (error != nullptr) {
                *error = err.message;
            }
            return false;
        }
        std::string trimmed = plugincore_detail::trimTrailingNewlines(output);
        if (!trimmed.empty()) {
            if (error != nullptr) {
                *error = trimmed;
            }
            return false;
        }

        invalidatePathAndParent(wfxFrom);
        invalidatePathAndParent(wfxTo);
        return true;
    }

    bool setModificationTime(const std::string& wfxRemote, int64_t mtime, std::string* error) {
        if (error != nullptr) {
            error->clear();
        }
        RemotePath rp = parseWfxPath(wfxRemote);

        // `touch -c` is deliberately silent about a file that isn't
        // there, and this transport carries no exit status -- so
        // "produced no output" would otherwise read as success for a path
        // that was never touched. -c is still what gets sent (dropping it
        // would make touch CREATE the missing file, which is worse); the
        // existence question is answered here instead.
        DirEntry info;
        bool exists = false;
        AdbError statErr = client_.syncStat(rp.serial, rp.path, &info, &exists);
        if (!statErr.ok) {
            if (error != nullptr) {
                *error = statErr.message;
            }
            return false;
        }
        if (!exists) {
            if (error != nullptr) {
                *error = "cannot set the time of " + rp.path + ": no such file or directory";
            }
            return false;
        }

        std::string quoted = shellQuote(rp.path);

        std::string epochCommand = "touch -c -d @" + std::to_string(mtime) + " " + quoted;
        std::string epochOutput;
        AdbError epochErr = client_.shellCommand(rp.serial, epochCommand, &epochOutput);
        if (!epochErr.ok) {
            if (error != nullptr) {
                *error = epochErr.message;
            }
            return false;
        }
        if (plugincore_detail::trimTrailingNewlines(epochOutput).empty()) {
            invalidatePathAndParent(wfxRemote);
            return true;
        }

        // Fallback: Android's toybox touch has historically rejected the
        // "-d @<epoch>" form.
        std::string tTimestamp;
        if (!plugincore_detail::formatTouchTArg(mtime, &tTimestamp)) {
            if (error != nullptr) {
                *error = "modification time is out of range";
            }
            return false;
        }
        // TZ=UTC is not decoration. POSIX touch -t interprets its
        // argument in the *shell's* local time, and that shell is on the
        // phone -- so a phone set to UTC+3 would land every fallback
        // stamp three hours off. formatTouchTArg deliberately formats in
        // UTC (gmtime_r); this assignment prefix, which toybox's sh
        // honours like any other, is what makes the two agree.
        std::string tCommand = "TZ=UTC touch -c -t " + tTimestamp + " " + quoted;
        std::string tOutput;
        AdbError tErr = client_.shellCommand(rp.serial, tCommand, &tOutput);
        if (!tErr.ok) {
            if (error != nullptr) {
                *error = tErr.message;
            }
            return false;
        }
        std::string trimmed = plugincore_detail::trimTrailingNewlines(tOutput);
        if (!trimmed.empty()) {
            if (error != nullptr) {
                *error = trimmed;
            }
            return false;
        }

        invalidatePathAndParent(wfxRemote);
        return true;
    }

    ListingCache& cache() {
        return cache_;
    }

private:
    // parseRemotePath alone takes the WFX path's first component as the
    // literal serial -- it has no idea about the "<model> (<serial>)"
    // display names listDirectory("/") invents for the root entries. Every
    // operation that turns a WFX path into a device to talk to MUST go
    // through this instead of calling parseRemotePath directly, or a
    // device with a known model can never be opened (host:transport:
    // would be sent the display name, not the serial). A no-op for a bare
    // serial, so nothing that already worked regresses.
    static RemotePath parseWfxPath(const std::string& wfxPath) {
        RemotePath rp = parseRemotePath(wfxPath);
        rp.serial = serialFromDisplayName(rp.serial);
        return rp;
    }

    bool listRootDevices(std::vector<FindResult>* out, std::string* warning, std::string* error) {
        std::vector<DeviceInfo> devices;
        AdbError err = client_.listDevices(&devices);
        if (!err.ok) {
            if (error != nullptr) {
                *error = err.message;
            }
            return false;
        }

        std::string warnings;
        for (const DeviceInfo& d : devices) {
            if (!deviceStateIsUsable(d.state)) {
                if (!warnings.empty()) {
                    warnings += "\n";
                }
                warnings += deviceStateMessage(d);
                continue;
            }
            FindResult r;
            r.name = displayNameForDevice(d);
            r.isDir = true;
            out->push_back(std::move(r));
        }
        if (warning != nullptr) {
            *warning = warnings;
        }
        return true;
    }

    // Rejects, with a message that says which, a listing target that does
    // not exist or is not a directory. A symlink is resolved through
    // before being judged (see resolveSymlinkTargets): /sdcard is a
    // symlink on every modern device and must of course still open.
    bool checkIsDirectory(const std::string& serial, const std::string& path, std::string* error) {
        DirEntry info;
        bool exists = false;
        AdbError err = client_.syncStat(serial, path, &info, &exists);
        if (!err.ok) {
            if (error != nullptr) {
                *error = err.message;
            }
            return false;
        }
        if (!exists) {
            if (error != nullptr) {
                *error = "cannot open " + path + ": no such file or directory";
            }
            return false;
        }
        if (info.isDir()) {
            return true;
        }
        if (info.isSymlink()) {
            DirEntry target;
            bool targetExists = false;
            AdbError targetErr =
                client_.syncStat(serial, throughSymlink(path), &target, &targetExists);
            if (targetErr.ok && targetExists && target.isDir()) {
                return true;
            }
        }
        if (error != nullptr) {
            *error = "cannot open " + path + ": not a directory";
        }
        return false;
    }

    // A path that lstat() resolves through a final symlink: POSIX
    // requires a trailing "/." to be resolved as a directory reference,
    // so lstat("/sdcard/.") reports the directory /sdcard points at
    // rather than the link. This is how a sync-protocol client asks
    // "what is on the other end of this link?" without a readlink
    // primitive (there is none in sync v1).
    static std::string throughSymlink(const std::string& path) {
        return joinWfxPath(path, ".");
    }

    // adbd's do_list uses lstat, so on any modern Android device the
    // device root comes back with sdcard, etc, d, vendor, bin, odm and
    // product all reported as 0120777 symlinks -- and /sdcard, the single
    // most common destination on the phone, renders as a 21-byte file
    // that cannot be opened. Re-stat each symlink through a trailing "/."
    // and record whether it points at a directory.
    //
    // Best-effort by construction: a stat that fails, or a link that
    // dangles, simply leaves the entry classified as a non-directory --
    // the listing itself must never fail because one link could not be
    // resolved. The cost is bounded and paid where it matters: roughly
    // fifteen links in "/", effectively none under /sdcard.
    void resolveSymlinkTargets(const std::string& serial, const std::string& dirPath,
                               std::vector<DirEntry>* entries) {
        for (DirEntry& e : *entries) {
            if (!e.isSymlink() || e.name == "." || e.name == "..") {
                continue;
            }
            DirEntry target;
            bool exists = false;
            AdbError err = client_.syncStat(
                serial, throughSymlink(joinWfxPath(dirPath, e.name)), &target, &exists);
            if (err.ok && exists && target.isDir()) {
                e.symlinkTargetIsDir = true;
            }
        }
    }

    static void appendEntries(const std::vector<DirEntry>& entries, std::vector<FindResult>* out) {
        for (const DirEntry& e : entries) {
            if (e.name == "." || e.name == "..") {
                continue;
            }
            FindResult r;
            r.name = e.name;
            r.size = e.size;
            r.mtime = e.mtime;
            r.unixMode = e.mode;
            // A symlink to a directory is navigable as one (see
            // resolveSymlinkTargets); unixMode still carries the link's
            // own S_IFLNK bits so DC shows what it really is.
            r.isDir = e.isNavigableDir();
            out->push_back(std::move(r));
        }
    }

    bool runMutatingShellCommand(const RemotePath& rp, const std::string& command,
                                 const std::string& wfxPath, std::string* error) {
        if (error != nullptr) {
            error->clear();
        }
        std::string output;
        AdbError err = client_.shellCommand(rp.serial, command, &output);
        if (!err.ok) {
            if (error != nullptr) {
                *error = err.message;
            }
            return false;
        }
        std::string trimmed = plugincore_detail::trimTrailingNewlines(output);
        if (!trimmed.empty()) {
            if (error != nullptr) {
                *error = trimmed;
            }
            return false;
        }
        invalidatePathAndParent(wfxPath);
        return true;
    }

    // Drops wfxPath itself (in case it was cached as a directory listing)
    // and its parent directory's listing (whose contents just changed) --
    // "the affected directories" every mutating operation must invalidate.
    void invalidatePathAndParent(const std::string& wfxPath) {
        cache_.invalidate(wfxPath);
        cache_.invalidate(plugincore_detail::parentWfxDir(wfxPath));
    }

    AdbClient& client_;
    ListingCache cache_;
};

#endif // ADB_WFX_FSPLUGIN_IMPL_HPP
