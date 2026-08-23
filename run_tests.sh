#!/usr/bin/env bash
# The full unit suite. Needs no Android device -- everything below runs
# against tests/fake_transport.hpp. Ends with ALL CHECKS PASSED.
#
# The device-backed suites are separate and opt-in: tests/device_test.sh
# (ADB_WFX_DEVICE_TESTS=1) and tests/bench.sh.
set -euo pipefail

cd "$(dirname "$0")"

mkdir -p tests/bin

# Build the plugin first so tests/test_exports.cpp's dlopen test exercises
# the real thing rather than its SKIP path.
./build.sh

OS="$(uname -s)"
if [ -z "${CXX:-}" ]; then
    case "$OS" in
        Darwin) CXX=clang++ ;;
        *)      CXX=g++ ;;
    esac
fi

TEST_FLAGS=(-std=c++17 -Wall -Wextra -Werror -pthread -I.)
TEST_LIBS=()
if [ "$OS" = "Linux" ]; then
    # tests/test_exports.cpp dlopen()s the built plugin; glibc older than 2.34
    # keeps dlopen in libdl rather than libc.
    TEST_LIBS+=(-ldl)
fi

"$CXX" "${TEST_FLAGS[@]}" tests/main.cpp tests/test_*.cpp "${TEST_LIBS[@]}" -o tests/bin/run_tests

./tests/bin/run_tests

./scripts/check-exports.sh

# tests/device_driver.cpp has its own main() and is never swept into the
# tests/test_*.cpp glob above (see the file's own header comment).
# Compiling it needs no device (only *running* it does -- that's
# tests/device_test.sh's job, opt-in via ADB_WFX_DEVICE_TESTS=1), so it
# is built here unconditionally, as part of the normal green build: a
# future change to fsplugin_impl.hpp/adbclient.hpp that broke this file
# must fail -Werror right here, not go unnoticed until someone next has a
# phone plugged in.
echo "Building tests/device_driver.cpp..."
"$CXX" "${TEST_FLAGS[@]}" tests/device_driver.cpp "${TEST_LIBS[@]}" -o tests/bin/device_driver

echo "ALL CHECKS PASSED"
