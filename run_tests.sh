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

echo "ALL CHECKS PASSED"
