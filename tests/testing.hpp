// Minimal self-registering test framework. No external dependencies.
#ifndef ADB_WFX_TESTING_HPP
#define ADB_WFX_TESTING_HPP

#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct TestCase {
    std::string suite;
    std::string name;
    void (*fn)();
};

// Function-local static avoids static-init-order problems across TUs:
// every TU that registers a test gets the same registry no matter which
// global constructor runs first.
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct TestStats {
    int passed = 0;
    int failed = 0;
};

inline TestStats& testStats() {
    static TestStats stats;
    return stats;
}

inline std::string& currentSuite() {
    static std::string suite;
    return suite;
}

inline std::string& currentName() {
    static std::string name;
    return name;
}

inline void reportSuccess() {
    ++testStats().passed;
}

inline void reportFailure(const char* file, int line, const std::string& expr) {
    std::fprintf(stderr, "FAIL %s.%s at %s:%d: %s\n",
                 currentSuite().c_str(), currentName().c_str(),
                 file, line, expr.c_str());
    ++testStats().failed;
}

#define CHECK(expr) \
    do { \
        if (expr) { reportSuccess(); } \
        else { reportFailure(__FILE__, __LINE__, #expr); } \
    } while (0)

#define CHECK_EQ(a, b) \
    do { \
        if ((a) == (b)) { \
            reportSuccess(); \
        } else { \
            std::ostringstream failMsg; \
            failMsg << #a << " == " << #b << " (got " << (a) << ", expected " << (b) << ")"; \
            reportFailure(__FILE__, __LINE__, failMsg.str()); \
        } \
    } while (0)

#define CHECK_STR_EQ(a, b) \
    do { \
        std::string lhsStr = (a); \
        std::string rhsStr = (b); \
        if (lhsStr == rhsStr) { \
            reportSuccess(); \
        } else { \
            reportFailure(__FILE__, __LINE__, \
                #a " == " #b " (got \"" + lhsStr + "\", expected \"" + rhsStr + "\")"); \
        } \
    } while (0)

#define CHECK_THROWS(expr) \
    do { \
        bool threw = false; \
        try { \
            (void)(expr); \
        } catch (...) { \
            threw = true; \
        } \
        if (threw) { reportSuccess(); } \
        else { reportFailure(__FILE__, __LINE__, #expr " (did not throw)"); } \
    } while (0)

#define TEST(suiteName, testName) \
    struct Test_##suiteName##_##testName { \
        static void run(); \
        Test_##suiteName##_##testName() { \
            registry().push_back(TestCase{#suiteName, #testName, &run}); \
        } \
    }; \
    static Test_##suiteName##_##testName registerTest_##suiteName##_##testName; \
    void Test_##suiteName##_##testName::run()

inline int runAllTests() {
    for (const TestCase& testCase : registry()) {
        currentSuite() = testCase.suite;
        currentName() = testCase.name;
        testCase.fn();
    }
    std::printf("%d passed, %d failed\n", testStats().passed, testStats().failed);
    return testStats().failed == 0 ? 0 : 1;
}

#endif // ADB_WFX_TESTING_HPP
