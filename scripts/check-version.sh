#!/usr/bin/env bash
# Asserts that version.h and CHANGELOG.md tell the same story, and -- when a
# tag is being released -- that the tag agrees with both.
#
#   ./scripts/check-version.sh            just the two files
#   ./scripts/check-version.sh v1.2.3     also require the tag to match
set -euo pipefail

cd "$(dirname "$0")/.."

VERSION="$(sed -n 's/^#define ADB_WFX_VERSION "\(.*\)"$/\1/p' version.h)"
if [ -z "$VERSION" ]; then
    echo "check-version.sh: could not read ADB_WFX_VERSION from version.h" >&2
    exit 1
fi

# The newest released section in the changelog, skipping [Unreleased].
CHANGELOG_VERSION="$(grep -m1 -E '^## \[[0-9]' CHANGELOG.md | sed -E 's/^## \[([^]]+)\].*/\1/')"
if [ "$VERSION" != "$CHANGELOG_VERSION" ]; then
    echo "check-version.sh FAILED: version.h says $VERSION," >&2
    echo "  but the newest CHANGELOG.md section is $CHANGELOG_VERSION." >&2
    exit 1
fi

if [ $# -ge 1 ]; then
    TAG="${1#v}"
    # A prerelease tag (v1.0.0-rc1) releases the version in version.h.
    if [ "${TAG%%-*}" != "$VERSION" ]; then
        echo "check-version.sh FAILED: tag $1 does not match version.h ($VERSION)." >&2
        echo "  Bump version.h and CHANGELOG.md, or retag." >&2
        exit 1
    fi
fi

echo "check-version.sh: version $VERSION is consistent${1:+ with tag $1}."
