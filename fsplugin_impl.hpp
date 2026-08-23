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
// correctness bug this whole project exists to eliminate.
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

        if (const std::vector<DirEntry>* cached = cache_.get(wfxDir)) {
            appendEntries(*cached, out);
            return true;
        }

        std::vector<DirEntry> entries;
        AdbError err = client_.syncList(rp.serial, rp.path, &entries);
        if (!err.ok) {
            if (error != nullptr) {
                *error = err.message;
            }
            return false;
        }

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

    bool renameOrMove(const std::string& wfxFrom, const std::string& wfxTo, bool move,
                      bool overwrite, std::string* error) {
        // A same-directory rename and a cross-directory move both land on
        // the same "mv" against the device's single filesystem tree --
        // the distinction only matters to Double Commander's UI, never to
        // the shell command issued here.
        (void)move;

        if (error != nullptr) {
            error->clear();
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
            return false;
        }

        std::string command = "mv ";
        if (!overwrite) {
            command += "-n ";
        }
        command += shellQuote(rpFrom.path) + " " + shellQuote(rpTo.path);

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
        std::string tCommand = "touch -c -t " + tTimestamp + " " + quoted;
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
            r.isDir = e.isDir(); // symlinks: isDir() is false, mode keeps the S_IFLNK bits
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
