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
#include <map>
#include <string>
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

// Caches directory listings keyed by WFX directory path, so repeated
// browsing of the same directory doesn't re-issue a sync LIST round trip.
class ListingCache {
public:
    void put(const std::string& wfxDir, std::vector<DirEntry> entries) {
        entries_[wfxDir] = std::move(entries);
    }

    const std::vector<DirEntry>* get(const std::string& wfxDir) const {
        auto it = entries_.find(wfxDir);
        return (it != entries_.end()) ? &it->second : nullptr;
    }

    // Drops wfxDir and everything beneath it. A real prefix-on-path-
    // boundary check: "/S/sdcard" invalidates "/S/sdcard" and
    // "/S/sdcard/DCIM" but leaves "/S/sdcardX" alone.
    void invalidate(const std::string& wfxDir) {
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
        entries_.clear();
    }

    size_t size() const {
        return entries_.size();
    }

private:
    std::map<std::string, std::vector<DirEntry>> entries_;
};

#endif // ADB_WFX_ADBUTILS_HPP
