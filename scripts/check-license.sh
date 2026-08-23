#!/usr/bin/env bash
# Fails if any forbidden LGPL/GPL-associated text is found in the tree.
# Hits are only allowed in README.md (which may legitimately discuss the
# LGPL situation in prose). Excludes .git, .superpowers, tests/bin, the
# plan and the design spec (which discuss it too), and this script itself
# (whose own pattern text would otherwise match).
set -euo pipefail

cd "$(dirname "$0")/.."

PATTERN='Lesser General Public|ivanenko|Sultanov|GNU General Public'
violations=""

while IFS= read -r -d '' file; do
    # -i: the spec asked for a case-insensitive search. The pattern is
    # deliberately NOT broadened to a bare "GPL" -- fsplugin.cpp's
    # gPluginNr would match that and fail the check for nothing.
    if grep -Eqi "$PATTERN" "$file"; then
        if [ "$(basename "$file")" != "README.md" ]; then
            violations="${violations}${file}"$'\n'
        fi
    fi
done < <(find . \( \
            -path './.git' -o \
            -path './.superpowers' -o \
            -path './tests/bin' -o \
            -path './docs/plan-adb-wfx.md' -o \
            -path './scalable-cuddling-crown.md' -o \
            -path './scripts/check-license.sh' \
        \) -prune -o -type f -print0)

if [ -n "$violations" ]; then
    echo "License check FAILED: forbidden license text found in:" >&2
    printf '%s' "$violations" >&2
    exit 1
fi

echo "License check passed."
