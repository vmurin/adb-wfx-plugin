#!/usr/bin/env bash
# Asserts that every architecture slice of fsplugin.wfx64 declares macOS 14.0
# as its minimum -- the floor the README promises.
#
# build.sh pins it with -mmacosx-version-min. Without that pin clang takes the
# minimum from the SDK of the machine that compiles, so a CI runner moving to a
# newer macOS image would quietly stop the release from loading on older
# systems; nothing else would fail. This is the check that does.
#
# macOS only: the minimum is a Mach-O property (LC_BUILD_VERSION).
set -euo pipefail

cd "$(dirname "$0")/.."

EXPECTED_MINOS="14.0"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "check-macos-target.sh: not macOS, nothing to check."
    exit 0
fi

if [ ! -f fsplugin.wfx64 ]; then
    ./build.sh
fi

# vtool and lipo ship with the Xcode command line tools, as does the compiler
# that produced the file.
FAILED=0
for arch in $(lipo -archs fsplugin.wfx64); do
    minos="$(vtool -arch "$arch" -show-build fsplugin.wfx64 \
        | awk '$1 == "minos" { print $2; exit }')"
    if [ "$minos" = "$EXPECTED_MINOS" ]; then
        echo "check-macos-target.sh: $arch minos $minos"
    else
        echo "check-macos-target.sh FAILED: $arch declares minos" \
             "'${minos:-<none>}', expected $EXPECTED_MINOS." >&2
        FAILED=1
    fi
done

if [ "$FAILED" -ne 0 ]; then
    echo "  build.sh passes -mmacosx-version-min; if the floor moved on purpose," >&2
    echo "  update EXPECTED_MINOS here and the README's Supported platforms." >&2
    exit 1
fi
