#!/usr/bin/env bash
set -euo pipefail

mkdir -p tests/bin

# Build the plugin first so tests/test_exports.cpp's dlopen test exercises
# the real thing rather than its SKIP path (see task-9-brief.md).
./compile_mac.sh

clang++ -std=c++17 -Wall -Wextra -Werror -pthread -I. tests/main.cpp tests/test_*.cpp -o tests/bin/run_tests

./tests/bin/run_tests

./scripts/check-exports.sh

./scripts/check-license.sh

# tests/device_driver.cpp has its own main() and is never swept into the
# tests/test_*.cpp glob above (see the file's own header comment).
# Compiling it needs no device (only *running* it does -- that's
# tests/device_test.sh's job, opt-in via ADB_WFX_DEVICE_TESTS=1), so it
# is built here unconditionally, as part of the normal green build: a
# future change to fsplugin_impl.hpp/adbclient.hpp that broke this file
# must fail -Werror right here, not go unnoticed until someone next has a
# phone plugged in.
echo "Building tests/device_driver.cpp..."
clang++ -std=c++17 -Wall -Wextra -Werror -I. tests/device_driver.cpp -o tests/bin/device_driver

echo "ALL CHECKS PASSED"
