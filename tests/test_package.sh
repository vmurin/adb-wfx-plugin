#!/usr/bin/env bash
# tests/test_package.sh -- covers scripts/package.sh: what a release archive
# contains, and the two properties of it that are invisible until someone is
# already in trouble.
#
#   1. pluginst.inf sits at the archive ROOT. Double Commander and Total
#      Commander look for it there when deciding whether a zip is an
#      installable plugin; wrap the contents in a directory and the archive
#      silently degrades into a plain zip.
#   2. The archive says which platform it is built for. Every one of them holds
#      a file called fsplugin.wfx64, and once unpacked the zip's name -- the
#      only thing that said linux or macos -- is gone.
#
# Packs under a suffix of its own so a real dist/ artifact is never touched.
set -euo pipefail

cd "$(dirname "$0")/.."
REPO="$PWD"

if ! command -v zip >/dev/null 2>&1 || ! command -v unzip >/dev/null 2>&1; then
    echo "test_package.sh: SKIPPED -- zip/unzip not on this machine."
    exit 0
fi
if [ ! -f fsplugin.wfx64 ]; then
    echo "test_package.sh: SKIPPED -- fsplugin.wfx64 not built."
    exit 0
fi

SUFFIX="selftest-platform"
VERSION="$(sed -n 's/^#define ADB_WFX_VERSION "\(.*\)"$/\1/p' version.h)"
ZIP="$REPO/dist/adb-wfx-$VERSION-$SUFFIX.zip"
trap 'rm -f "$ZIP"' EXIT

CHECKS=0
FAILURES=0
pass() { CHECKS=$((CHECKS + 1)); }
fail() {
    CHECKS=$((CHECKS + 1))
    FAILURES=$((FAILURES + 1))
    echo "  FAIL: $*" >&2
}
assert_contains() { # desc haystack needle
    case "$2" in
        *"$3"*) pass ;;
        *) fail "$1: [$3] not present" ;;
    esac
}
assert_missing() { # desc haystack needle
    case "$2" in
        *"$3"*) fail "$1: [$3] should not be present" ;;
        *) pass ;;
    esac
}

echo "test_package.sh: package.sh"

./scripts/package.sh "$SUFFIX" >/dev/null

# Paths exactly as stored, which is what makes the "at the root" claim below
# mean something: a wrapping directory would show up here as a path prefix.
NAMES="$(unzip -Z1 "$ZIP")"

for f in pluginst.inf fsplugin.wfx64 install.sh register_plugin.py \
         README.md LICENSE CHANGELOG.md "PLATFORM-$SUFFIX.txt"; do
    assert_contains "archive holds $f at the root" "$(printf '\n%s\n' "$NAMES")" "$(printf '\n%s\n' "$f")"
done

# Nothing may sit inside a directory: Double Commander stops recognising the
# archive as an installable plugin the moment pluginst.inf is not at the root.
if printf '%s\n' "$NAMES" | grep -q '/'; then
    fail "archive has a wrapping directory: $(printf '%s\n' "$NAMES" | grep '/' | head -1)"
else
    pass
fi

INF="$(unzip -p "$ZIP" pluginst.inf)"
assert_missing "pluginst.inf: no unsubstituted placeholder" "$INF" "@VERSION@"
assert_missing "pluginst.inf: no unsubstituted placeholder" "$INF" "@PLATFORM@"
assert_contains "pluginst.inf: names the version" "$INF" "$VERSION"
assert_contains "pluginst.inf: names the platform" "$INF" "$SUFFIX"
assert_contains "pluginst.inf: still a wfx plugin" "$INF" "type=wfx"
assert_contains "pluginst.inf: still names the binary" "$INF" "file=fsplugin.wfx64"

MARKER="$(unzip -p "$ZIP" "PLATFORM-$SUFFIX.txt")"
assert_contains "marker: names this platform" "$MARKER" "$SUFFIX"
assert_contains "marker: names the version" "$MARKER" "$VERSION"
assert_contains "marker: names the other archives" "$MARKER" "macos-universal.zip"
assert_contains "marker: names the symptom" "$MARKER" "This is not a valid plugin!"

if [ "$FAILURES" -gt 0 ]; then
    echo "test_package.sh: $FAILURES of $CHECKS checks FAILED" >&2
    exit 1
fi
echo "test_package.sh: $CHECKS checks passed"
