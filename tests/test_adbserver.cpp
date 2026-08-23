// Tests for adbserver.hpp: choosing the ADB server port and locating the
// `adb` binary. Everything here is driven through the injected `getEnv` /
// `isExecutable` seams -- no test reads the real environment or the real
// filesystem. The one exception, startAdbServer, is only exercised against
// a path that cannot possibly exist, per the task brief.
#include "adbserver.hpp"
#include "testing.hpp"

#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace {

// Builds a getEnv seam from a fixed map; returns nullptr for anything not
// in the map, matching real getenv() semantics for an unset variable.
std::function<const char*(const char*)> envFrom(const std::map<std::string, std::string>& vars) {
    return [vars](const char* name) -> const char* {
        auto it = vars.find(name);
        return it == vars.end() ? nullptr : it->second.c_str();
    };
}

} // namespace

// ---------------------------------------------------------------------
// adbServerPort
// ---------------------------------------------------------------------

TEST(AdbServerPortSuite, noEnvFallsBackToDefault) {
    auto getEnv = envFrom({});
    CHECK_EQ(adbServerPort(getEnv), ADB_DEFAULT_PORT);
}

TEST(AdbServerPortSuite, validValueIsUsed) {
    auto getEnv = envFrom({{"ANDROID_ADB_SERVER_PORT", "5038"}});
    CHECK_EQ(adbServerPort(getEnv), 5038);
}

TEST(AdbServerPortSuite, zeroFallsBackToDefault) {
    auto getEnv = envFrom({{"ANDROID_ADB_SERVER_PORT", "0"}});
    CHECK_EQ(adbServerPort(getEnv), ADB_DEFAULT_PORT);
}

TEST(AdbServerPortSuite, negativeFallsBackToDefault) {
    auto getEnv = envFrom({{"ANDROID_ADB_SERVER_PORT", "-1"}});
    CHECK_EQ(adbServerPort(getEnv), ADB_DEFAULT_PORT);
}

TEST(AdbServerPortSuite, tooLargeFallsBackToDefault) {
    auto getEnv = envFrom({{"ANDROID_ADB_SERVER_PORT", "70000"}});
    CHECK_EQ(adbServerPort(getEnv), ADB_DEFAULT_PORT);
}

TEST(AdbServerPortSuite, nonNumericFallsBackToDefault) {
    auto getEnv = envFrom({{"ANDROID_ADB_SERVER_PORT", "abc"}});
    CHECK_EQ(adbServerPort(getEnv), ADB_DEFAULT_PORT);
}

TEST(AdbServerPortSuite, emptyFallsBackToDefault) {
    auto getEnv = envFrom({{"ANDROID_ADB_SERVER_PORT", ""}});
    CHECK_EQ(adbServerPort(getEnv), ADB_DEFAULT_PORT);
}

TEST(AdbServerPortSuite, trailingGarbageFallsBackToDefault) {
    auto getEnv = envFrom({{"ANDROID_ADB_SERVER_PORT", "5038x"}});
    CHECK_EQ(adbServerPort(getEnv), ADB_DEFAULT_PORT);
}

// ---------------------------------------------------------------------
// adbBinaryCandidates
// ---------------------------------------------------------------------

TEST(AdbBinaryCandidatesSuite, pathElementsComeBeforeFallbacks) {
    auto getEnv = envFrom({{"PATH", "/a:/b"}});
    std::vector<std::string> candidates = adbBinaryCandidates(getEnv);

    CHECK(candidates.size() >= 2);
    CHECK_STR_EQ(candidates[0], "/a/adb");
    CHECK_STR_EQ(candidates[1], "/b/adb");

    // The hardcoded fallbacks still appear, after the PATH entries.
    bool foundHomebrewShare = false;
    for (const std::string& c : candidates) {
        if (c == "/opt/homebrew/share/android-commandlinetools/platform-tools/adb") {
            foundHomebrewShare = true;
        }
    }
    CHECK(foundHomebrewShare);
}

TEST(AdbBinaryCandidatesSuite, adbPathEnvComesFirst) {
    auto getEnv = envFrom({{"ADB_PATH", "/custom/adb"}, {"PATH", "/a"}});
    std::vector<std::string> candidates = adbBinaryCandidates(getEnv);

    CHECK(!candidates.empty());
    CHECK_STR_EQ(candidates[0], "/custom/adb");
}

TEST(AdbBinaryCandidatesSuite, emptyPathStillYieldsFallbacks) {
    auto getEnv = envFrom({{"PATH", ""}});
    std::vector<std::string> candidates = adbBinaryCandidates(getEnv);

    bool foundLocalBin = false;
    for (const std::string& c : candidates) {
        if (c == "/usr/local/bin/adb") {
            foundLocalBin = true;
        }
    }
    CHECK(foundLocalBin);
}

TEST(AdbBinaryCandidatesSuite, noPathAtAllStillYieldsFallbacks) {
    auto getEnv = envFrom({});
    std::vector<std::string> candidates = adbBinaryCandidates(getEnv);
    CHECK(!candidates.empty());
    CHECK_STR_EQ(candidates.back(), "/usr/local/share/android-commandlinetools/platform-tools/adb");
}

TEST(AdbBinaryCandidatesSuite, duplicatePathEntriesDoNotDuplicateCandidates) {
    auto getEnv = envFrom({{"PATH", "/a:/b:/a"}});
    std::vector<std::string> candidates = adbBinaryCandidates(getEnv);

    int countA = 0;
    for (const std::string& c : candidates) {
        if (c == "/a/adb") {
            ++countA;
        }
    }
    CHECK_EQ(countA, 1);
    CHECK_STR_EQ(candidates[0], "/a/adb");
    CHECK_STR_EQ(candidates[1], "/b/adb");
}

TEST(AdbBinaryCandidatesSuite, emptyPathElementIsSkippedNotTurnedIntoRootAdb) {
    auto getEnv = envFrom({{"PATH", "/a::/b"}});
    std::vector<std::string> candidates = adbBinaryCandidates(getEnv);

    for (const std::string& c : candidates) {
        CHECK(c != "/adb");
    }
    CHECK_STR_EQ(candidates[0], "/a/adb");
    CHECK_STR_EQ(candidates[1], "/b/adb");
}

TEST(AdbBinaryCandidatesSuite, homeExpansionUsedForSdkFallback) {
    auto getEnv = envFrom({{"HOME", "/Users/tester"}});
    std::vector<std::string> candidates = adbBinaryCandidates(getEnv);

    bool foundSdkPath = false;
    for (const std::string& c : candidates) {
        if (c == "/Users/tester/Library/Android/sdk/platform-tools/adb") {
            foundSdkPath = true;
        }
    }
    CHECK(foundSdkPath);
}

// ---------------------------------------------------------------------
// findAdbBinary
// ---------------------------------------------------------------------

TEST(FindAdbBinarySuite, returnsFirstAcceptedCandidate) {
    auto getEnv = envFrom({{"PATH", "/a:/b"}});
    auto isExecutable = [](const std::string& p) { return p == "/b/adb"; };
    CHECK_STR_EQ(findAdbBinary(getEnv, isExecutable), "/b/adb");
}

TEST(FindAdbBinarySuite, returnsEmptyWhenNothingAccepted) {
    auto getEnv = envFrom({{"PATH", "/a:/b"}});
    auto isExecutable = [](const std::string&) { return false; };
    CHECK_STR_EQ(findAdbBinary(getEnv, isExecutable), "");
}

TEST(FindAdbBinarySuite, adbPathPreferredWhenExecutable) {
    auto getEnv = envFrom({{"ADB_PATH", "/custom/adb"}, {"PATH", "/a"}});
    auto isExecutable = [](const std::string& p) { return p == "/custom/adb" || p == "/a/adb"; };
    CHECK_STR_EQ(findAdbBinary(getEnv, isExecutable), "/custom/adb");
}

// ---------------------------------------------------------------------
// startAdbServer
// ---------------------------------------------------------------------

TEST(StartAdbServerSuite, nonexistentBinaryReturnsFalse) {
    CHECK(!startAdbServer("/nonexistent/adb"));
}

// startAdbServer always execs "<adb> start-server", so there's no real
// binary we can point it at without touching the no-real-adb rule. These
// two exercise the waitpid/exit-status half of the function (unreached by
// the nonexistent-binary case, which fails at posix_spawn) using ordinary
// system binaries purely as stand-ins for "a process that exits 0" and
// "a process that exits nonzero" -- no adb server is started or implied.
TEST(StartAdbServerSuite, childExitingZeroReturnsTrue) {
    CHECK(startAdbServer("/usr/bin/true"));
}

TEST(StartAdbServerSuite, childExitingNonZeroReturnsFalse) {
    CHECK(!startAdbServer("/usr/bin/false"));
}
