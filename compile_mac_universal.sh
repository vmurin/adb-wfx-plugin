#!/usr/bin/env bash
# Kept as a wrapper around build.sh, which is where the compile line lives now.
set -euo pipefail
exec "$(dirname "$0")/build.sh" --universal "$@"
