// WFX remote path parsing/joining, single-quote shell escaping for the
// `shell:` service, and a directory listing cache. Pure UTF-8
// std::string; no I/O, no SDK types.
//
// Double Commander addresses everything under one virtual root: "/" lists
// attached devices, "/<serial>" is a device's filesystem root, and
// "/<serial>/sdcard/DCIM/a.jpg" is a real on-device path. parseRemotePath
// splits a WFX path into those two halves.
#ifndef ADB_WFX_ADBUTILS_HPP
#define ADB_WFX_ADBUTILS_HPP

#include "adbproto.hpp"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct RemotePath {
    bool isRoot = false;  // "/" -- the device list
    std::string serial;   // "" when isRoot
    std::string path;     // on-device absolute path, "/" for device root
};

inline RemotePath parseRemotePath(const std::string& wfxPath) {
    RemotePath result;

    if (wfxPath.empty() || wfxPath == "/") {
        result.isRoot = true;
        result.path = "/";
        return result;
    }

    // wfxPath always starts with '/'; the serial is the first component
    // after it, and the rest (if any) is the on-device absolute path.
    size_t start = (wfxPath.front() == '/') ? 1 : 0;
    size_t nextSlash = wfxPath.find('/', start);

    if (nextSlash == std::string::npos) {
        result.serial = wfxPath.substr(start);
        result.path = "/";
    } else {
        result.serial = wfxPath.substr(start, nextSlash - start);
        std::string rest = wfxPath.substr(nextSlash); // keeps the leading '/'
        result.path = (rest.size() > 1) ? rest : "/";
    }

    return result;
}

// Joins a directory path and a leaf name. WFX paths and on-device
// absolute paths share the same '/'-separated shape, so this serves both
// (PluginCore uses it for the on-device side when building a symlink's
// "<path>/." resolution target).
inline std::string joinWfxPath(const std::string& dir, const std::string& leaf) {
    if (!dir.empty() && dir.back() == '/') {
        return dir + leaf;
    }
    return dir + "/" + leaf;
}

// Single-quote wrapping for `shell:` arguments. Wraps arg in single quotes
// so the shell treats it as one literal word -- no expansion of $, `, *,
// whitespace, or anything else. An embedded single quote is closed out of
// the quoted string, escaped with a backslash, then the quoting resumes:
// it's -> 'it'\''s'. This is the only safe way to pass a POSIX shell an
// arbitrary byte string (including embedded newlines) as a single word.
inline std::string shellQuote(const std::string& arg) {
    std::string out;
    out.reserve(arg.size() + 2);
    out.push_back('\'');
    for (char c : arg) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

// How long a cached listing stays valid. Nothing but this plugin's own
// mutations invalidates an entry, so without an expiry a photo taken on
// the phone -- or a change made through Double Commander's other panel --
// would never appear: Ctrl+R would re-serve the same stale vector and the
// plugin would look broken. A few seconds is long enough to keep the real
// benefit (re-entering a directory you just left costs no round trip)
// and short enough that "refresh" means what the user thinks it means.
constexpr int64_t LISTING_CACHE_TTL_SECONDS = 5;

// Wall-clock source, in seconds since the Unix epoch. Injected so the
// expiry logic can be tested without sleeping.
using ClockFn = std::function<int64_t()>;

inline int64_t systemClockSeconds() {
    return static_cast<int64_t>(::time(nullptr));
}

// Caches directory listings keyed by WFX directory path, so repeated
// browsing of the same directory doesn't re-issue a sync LIST round trip.
// Entries expire after ttlSeconds.
//
// Every operation is serialised by a mutex. Double Commander calls a WFX
// plugin's file operations from a worker thread while the panel lists
// from another, and there is exactly one PluginCore holding exactly one
// of these -- racing put against get or invalidate on a bare std::map is
// node corruption, which takes the host process down rather than merely
// misbehaving. get() hands back a COPY for the same reason: a pointer
// into the map would outlive the lock that made it safe to obtain.
//
// This makes the shared container safe to touch from two threads. It does
// NOT make the plugin as a whole thread-safe -- see README's limitations
// note and fsplugin.cpp's file comment.
class ListingCache {
public:
    ListingCache() : clock_(&systemClockSeconds), ttlSeconds_(LISTING_CACHE_TTL_SECONDS) {}

    ListingCache(ClockFn clock, int64_t ttlSeconds)
        : clock_(std::move(clock)), ttlSeconds_(ttlSeconds) {}

    void put(const std::string& wfxDir, std::vector<DirEntry> entries) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_[wfxDir] = Entry{std::move(entries), clock_()};
    }

    // Copies the cached listing for wfxDir into *out and returns true, or
    // returns false (leaving *out alone) when there is no live entry.
    bool get(const std::string& wfxDir, std::vector<DirEntry>* out) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(wfxDir);
        if (it == entries_.end()) {
            return false;
        }
        // A wall clock can jump backwards (an NTP correction, a user
        // changing the system time), which would otherwise make an entry
        // look fresh for however far back it jumped. Any age outside
        // [0, ttl) counts as expired.
        int64_t age = clock_() - it->second.storedAt;
        if (age < 0 || age >= ttlSeconds_) {
            entries_.erase(it);
            return false;
        }
        *out = it->second.entries;
        return true;
    }

    // Drops wfxDir and everything beneath it. A real prefix-on-path-
    // boundary check: "/S/sdcard" invalidates "/S/sdcard" and
    // "/S/sdcard/DCIM" but leaves "/S/sdcardX" alone.
    void invalidate(const std::string& wfxDir) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string childPrefix = wfxDir + "/";
        for (auto it = entries_.begin(); it != entries_.end(); ) {
            bool isDirItself = it->first == wfxDir;
            bool isChild = it->first.compare(0, childPrefix.size(), childPrefix) == 0;
            if (isDirItself || isChild) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
    }

    // The number of entries held, expired ones included -- expiry is
    // lazy, applied by get().
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

private:
    struct Entry {
        std::vector<DirEntry> entries;
        int64_t storedAt = 0;
    };

    mutable std::mutex mutex_;
    ClockFn clock_;
    int64_t ttlSeconds_;
    std::map<std::string, Entry> entries_;
};

#endif // ADB_WFX_ADBUTILS_HPP
