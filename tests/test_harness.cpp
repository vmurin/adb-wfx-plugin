// Tests for the micro test harness itself (tests/testing.hpp).
#include "testing.hpp"

TEST(HarnessSuite, checkEqPassesOnEqualValues) {
    CHECK_EQ(2 + 2, 4);
}

TEST(HarnessSuite, checkStrEqPassesOnEqualStrings) {
    std::string a = "adb";
    std::string b = "adb";
    CHECK_STR_EQ(a, b);
}

TEST(HarnessSuite, checkThrowsCatchesThrownException) {
    CHECK_THROWS(throw std::runtime_error("boom"));
}

// Proves the harness can actually detect a failure: run a CHECK that is
// deliberately wrong against the real reporting sink, observe the failure
// counter increment, then restore it so the overall suite stays green.
// (This test intentionally prints one "FAIL ..." line to stderr while it
// runs -- that line is the evidence, not a bug.)
TEST(HarnessSuite, aFailingCheckIncrementsFailedCounter) {
    int before = testStats().failed;
    CHECK(1 == 2);
    int after = testStats().failed;
    testStats().failed = before; // restore so run_tests.sh still reports green
    CHECK_EQ(after, before + 1);
}
