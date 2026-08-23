// Test-only Transport double used by Tasks 6-9 to script multi-packet ADB
// conversations without a socket. Lives in tests/ so it never ships in the
// plugin. NOT compiled as its own translation unit (run_tests.sh globs only
// tests/test_*.cpp; this header is #included by whichever test files need it).
#ifndef ADB_WFX_TESTS_FAKE_TRANSPORT_HPP
#define ADB_WFX_TESTS_FAKE_TRANSPORT_HPP

#include "transport.hpp"

#include <cstring>
#include <deque>
#include <string>
#include <vector>

// A Transport double driven entirely by an in-memory script.
//
// Construct it with the bytes the "server" will hand back on readSome /
// readExactly -- either as one contiguous string, or as a list of chunks so
// a test can force short reads across a chunk boundary. Every byte the
// caller writes is appended to written(), so a test can assert on the exact
// bytes an AdbClient produced.
class FakeTransport : public Transport {
public:
    // All scripted reply bytes returned as a single chunk.
    explicit FakeTransport(const std::string& scriptedReplies) {
        if (!scriptedReplies.empty()) {
            readChunks_.push_back(scriptedReplies);
        }
    }

    // Scripted reply bytes handed back one chunk per readSome/readExactly
    // call boundary, so a test can force a short read at a chosen offset.
    explicit FakeTransport(const std::vector<std::string>& scriptedChunks)
        : readChunks_(scriptedChunks.begin(), scriptedChunks.end()) {}

    // Every byte ever passed to writeAll, in order.
    const std::string& written() const {
        return written_;
    }

    // After this many bytes have been read from the script, subsequent
    // reads fail (simulating a mid-transfer stream error) instead of
    // returning further scripted data.
    void failAfterBytes(size_t n) {
        failAfterBytes_ = n;
        failReadsEnabled_ = true;
    }

    // After this many bytes have been written, subsequent writes fail
    // (simulating a mid-transfer send error).
    void failWritesAfterBytes(size_t n) {
        failAfterWriteBytes_ = n;
        failWritesEnabled_ = true;
    }

    bool writeAll(const void* buf, size_t n) override {
        if (failWritesEnabled_ && bytesWritten_ + n > failAfterWriteBytes_) {
            lastError_ = "FakeTransport: simulated write failure";
            return false;
        }
        const char* p = static_cast<const char*>(buf);
        written_.append(p, n);
        bytesWritten_ += n;
        return true;
    }

    bool readExactly(void* buf, size_t n) override {
        char* out = static_cast<char*>(buf);
        size_t got = 0;
        while (got < n) {
            ptrdiff_t r = readSome(out + got, n - got);
            if (r <= 0) {
                return false;
            }
            got += static_cast<size_t>(r);
        }
        return true;
    }

    ptrdiff_t readSome(void* buf, size_t n) override {
        if (readChunks_.empty()) {
            lastError_ = "FakeTransport: read past end of script (EOF)";
            return 0;
        }
        if (failReadsEnabled_ && bytesRead_ >= failAfterBytes_) {
            lastError_ = "FakeTransport: simulated read failure";
            return -1;
        }

        std::string& front = readChunks_.front();
        size_t take = std::min(n, front.size());
        if (failReadsEnabled_ && bytesRead_ + take > failAfterBytes_) {
            take = failAfterBytes_ - bytesRead_;
        }
        std::memcpy(buf, front.data(), take);
        front.erase(0, take);
        if (front.empty()) {
            readChunks_.pop_front();
        }
        bytesRead_ += take;
        return static_cast<ptrdiff_t>(take);
    }

    void close() override {
        closed_ = true;
    }

    bool isClosed() const {
        return closed_;
    }

    std::string lastError() const override {
        return lastError_;
    }

private:
    std::deque<std::string> readChunks_;
    std::string written_;
    std::string lastError_;
    size_t bytesRead_ = 0;
    size_t bytesWritten_ = 0;
    size_t failAfterBytes_ = 0;
    bool failReadsEnabled_ = false;
    size_t failAfterWriteBytes_ = 0;
    bool failWritesEnabled_ = false;
    bool closed_ = false;
};

#endif // ADB_WFX_TESTS_FAKE_TRANSPORT_HPP
