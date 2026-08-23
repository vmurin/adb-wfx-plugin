// One entry from the ADB sync protocol's LIST response. Task 3 defines the
// struct because adbutils.hpp (ListingCache) needs it; Task 4 will add the
// wire-format parsing functions around it.
#ifndef ADB_WFX_ADBPROTO_HPP
#define ADB_WFX_ADBPROTO_HPP

#include <cstdint>
#include <string>

constexpr uint32_t POSIX_MODE_TYPE_MASK = 0170000;
constexpr uint32_t POSIX_MODE_DIRECTORY = 0040000;
constexpr uint32_t POSIX_MODE_SYMLINK = 0120000;

struct DirEntry {
    std::string name;
    uint32_t mode = 0;
    uint64_t size = 0;
    int64_t mtime = 0;

    bool isDir() const {
        return (mode & POSIX_MODE_TYPE_MASK) == POSIX_MODE_DIRECTORY;
    }

    bool isSymlink() const {
        return (mode & POSIX_MODE_TYPE_MASK) == POSIX_MODE_SYMLINK;
    }
};

#endif // ADB_WFX_ADBPROTO_HPP
