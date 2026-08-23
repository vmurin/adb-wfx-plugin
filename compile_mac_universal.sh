#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

[ -f fsplugin.cpp ] || { echo "fsplugin.cpp not present yet"; exit 1; }

clang++ -std=c++17 -Wall -Wextra -Werror -O2 -shared -fPIC -fvisibility=hidden -I. \
    -arch arm64 -arch x86_64 \
    fsplugin.cpp -o fsplugin.wfx64
