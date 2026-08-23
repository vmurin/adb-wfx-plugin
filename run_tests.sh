#!/usr/bin/env bash
set -euo pipefail

mkdir -p tests/bin

clang++ -std=c++17 -Wall -Wextra -Werror -I. tests/main.cpp tests/test_*.cpp -o tests/bin/run_tests

./tests/bin/run_tests

./scripts/check-license.sh

echo "ALL CHECKS PASSED"
