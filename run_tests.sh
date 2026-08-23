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
# tests/test_*.cpp glob above (see the file's own header comment). It is
# only compiled here -- never run, since that needs a real device -- so
# that it stays buildable with -Werror as part of the normal green build;
# actually exercising it against a phone is tests/device_test.sh's job,
# opt-in via the same env var.
if [ "${ADB_WFX_DEVICE_TESTS:-0}" = "1" ]; then
    echo "Building tests/device_driver.cpp (ADB_WFX_DEVICE_TESTS=1)..."
    clang++ -std=c++17 -Wall -Wextra -Werror -I. tests/device_driver.cpp -o tests/bin/device_driver
fi

echo "ALL CHECKS PASSED"
