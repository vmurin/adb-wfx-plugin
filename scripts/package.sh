#!/usr/bin/env bash
# scripts/package.sh <platform-suffix>
#
# Packs an already-built fsplugin.wfx64 into dist/adb-wfx-<version>-<suffix>.zip
# together with everything a user needs to install it. The archive carries a
# pluginst.inf, which is what makes Double Commander (and Total Commander)
# recognise it as an installable plugin rather than a plain zip.
set -euo pipefail

cd "$(dirname "$0")/.."

if [ $# -ne 1 ]; then
    echo "usage: scripts/package.sh <platform-suffix>   e.g. macos-universal" >&2
    exit 2
fi
SUFFIX="$1"

[ -f fsplugin.wfx64 ] || { echo "package.sh: fsplugin.wfx64 not built" >&2; exit 1; }

VERSION="$(sed -n 's/^#define ADB_WFX_VERSION "\(.*\)"$/\1/p' version.h)"
[ -n "$VERSION" ] || { echo "package.sh: cannot read version.h" >&2; exit 1; }

NAME="adb-wfx-${VERSION}-${SUFFIX}"
STAGE="dist/.stage-$NAME"

rm -rf "$STAGE"
mkdir -p "$STAGE"

cp fsplugin.wfx64 "$STAGE/"
cp README.md LICENSE CHANGELOG.md "$STAGE/"
cp scripts/install.sh "$STAGE/"
# install.sh looks for the registrar next to itself, which is the archive root
# here and scripts/ in a source checkout.
cp scripts/register_plugin.py "$STAGE/"
sed -e "s/@VERSION@/$VERSION/" -e "s/@PLATFORM@/$SUFFIX/" \
    packaging/pluginst.inf.in > "$STAGE/pluginst.inf"

# Every archive holds a file called fsplugin.wfx64, and an unpacked one is
# otherwise indistinguishable from the other two -- the zip name, which is the
# only thing that said "linux" or "macos", is gone by then, and "aarch64" reads
# as "Apple silicon" easily enough. So the platform gets its own name in the
# file listing, where it is answered without opening anything, and pluginst.inf
# carries it too: Double Commander offers to install a plugin straight from the
# zip, and that route never goes anywhere near install.sh or its checks.
cat >"$STAGE/PLATFORM-$SUFFIX.txt" <<MARKER
adb-wfx $VERSION -- this archive holds the $SUFFIX build, and no other.

A build for one platform cannot be loaded on another: Double Commander
rejects it with "This is not a valid plugin!" and says nothing about why.

  macOS, Apple silicon and Intel   adb-wfx-$VERSION-macos-universal.zip
  Linux on x86_64                  adb-wfx-$VERSION-linux-x86_64.zip
  Linux on ARM64                   adb-wfx-$VERSION-linux-aarch64.zip

https://github.com/vmurin/adb-wfx-plugin/releases/latest
MARKER

# Everything sits at the archive root, with no wrapping directory: that is
# where Double Commander and Total Commander look for pluginst.inf when
# deciding whether a zip is an installable plugin. A wrapping directory makes
# the archive open as a plain zip instead.
rm -f "dist/$NAME.zip"
( cd "$STAGE" && zip -q -r "../$NAME.zip" . )
rm -rf "$STAGE"

echo "package.sh: wrote dist/$NAME.zip"
