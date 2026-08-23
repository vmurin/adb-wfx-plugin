// Finding the adb binary and starting the adb server. This is the only
// place in the plugin (besides fsplugin.cpp itself) that decides *where*
// to look on the real machine; the lookup logic is exercised in tests
// entirely through the injected `getEnv` / `isExecutable` seams, and
// `startAdbServer` is the one function here that actually touches the OS
// (spawns a process). See docs/plan-adb-wfx.md (Task 6).
#ifndef ADB_WFX_ADBSERVER_HPP
#define ADB_WFX_ADBSERVER_HPP

#include "adbproto.hpp"

#include <cerrno>
#include <cstdlib>
#include <functional>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

// Reads ANDROID_ADB_SERVER_PORT; falls back to ADB_DEFAULT_PORT. A value
// is accepted only if it parses as a decimal integer in 1..65535 with no
// trailing garbage -- "0", "-1", "70000", "abc", "" and "5038x" all fall
// back.
inline int adbServerPort(const std::function<const char*(const char*)>& getEnv) {
    const char* raw = getEnv("ANDROID_ADB_SERVER_PORT");
    if (raw == nullptr || raw[0] == '\0') {
        return ADB_DEFAULT_PORT;
    }

    char* end = nullptr;
    long value = std::strtol(raw, &end, 10);
    if (end == raw || *end != '\0') {
        return ADB_DEFAULT_PORT;
    }
    if (value < 1 || value > 65535) {
        return ADB_DEFAULT_PORT;
    }
    return static_cast<int>(value);
}

namespace detail {

// Splits a ':'-separated PATH string into its elements, skipping empty
// elements (an empty PATH element must not turn into "/adb").
inline std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> elements;
    size_t start = 0;
    while (start <= path.size()) {
        size_t colon = path.find(':', start);
        std::string element =
            (colon == std::string::npos) ? path.substr(start) : path.substr(start, colon - start);
        if (!element.empty()) {
            elements.push_back(element);
        }
        if (colon == std::string::npos) {
            break;
        }
        start = colon + 1;
    }
    return elements;
}

} // namespace detail

// Candidate absolute paths for the adb binary, in priority order:
//   1. $ADB_PATH if set
//   2. each PATH element + "/adb"
//   3. /opt/homebrew/share/android-commandlinetools/platform-tools/adb
//   4. /opt/homebrew/bin/adb
//   5. /usr/local/bin/adb
//   6. $HOME/Library/Android/sdk/platform-tools/adb
//   7. /usr/local/share/android-commandlinetools/platform-tools/adb
// De-duplicates while preserving first-seen order.
inline std::vector<std::string> adbBinaryCandidates(
        const std::function<const char*(const char*)>& getEnv) {
    std::vector<std::string> candidates;
    std::vector<std::string> seen;

    auto addCandidate = [&](const std::string& candidate) {
        for (const std::string& s : seen) {
            if (s == candidate) {
                return;
            }
        }
        seen.push_back(candidate);
        candidates.push_back(candidate);
    };

    const char* adbPath = getEnv("ADB_PATH");
    if (adbPath != nullptr && adbPath[0] != '\0') {
        addCandidate(adbPath);
    }

    const char* pathEnv = getEnv("PATH");
    if (pathEnv != nullptr) {
        for (const std::string& element : detail::splitPath(pathEnv)) {
            addCandidate(element + "/adb");
        }
    }

    addCandidate("/opt/homebrew/share/android-commandlinetools/platform-tools/adb");
    addCandidate("/opt/homebrew/bin/adb");
    addCandidate("/usr/local/bin/adb");

    const char* home = getEnv("HOME");
    if (home != nullptr && home[0] != '\0') {
        addCandidate(std::string(home) + "/Library/Android/sdk/platform-tools/adb");
    }

    addCandidate("/usr/local/share/android-commandlinetools/platform-tools/adb");

    return candidates;
}

// Returns the first candidate for which isExecutable(p) is true, else "".
inline std::string findAdbBinary(
        const std::function<const char*(const char*)>& getEnv,
        const std::function<bool(const std::string&)>& isExecutable) {
    for (const std::string& candidate : adbBinaryCandidates(getEnv)) {
        if (isExecutable(candidate)) {
            return candidate;
        }
    }
    return "";
}

// Runs "<adb> start-server" via fork/exec (never system() with an
// unescaped string) and returns true on exit status 0. Returns false --
// never throws, never hangs -- when the binary does not exist or the
// spawn otherwise fails.
inline bool startAdbServer(const std::string& adbBinary) {
    char binaryArg[] = "start-server";
    char* argv[] = {const_cast<char*>(adbBinary.c_str()), binaryArg, nullptr};

    pid_t pid = 0;
    int spawnResult = posix_spawn(&pid, adbBinary.c_str(), nullptr, nullptr, argv, environ);
    if (spawnResult != 0) {
        return false;
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        // A signal delivered to this (long-lived GUI) process must not
        // leave the already-spawned child unreaped as a zombie -- retry
        // on EINTR specifically, give up on any other error.
        if (errno != EINTR) {
            return false;
        }
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

#endif // ADB_WFX_ADBSERVER_HPP
