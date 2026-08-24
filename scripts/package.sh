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
sed "s/@VERSION@/$VERSION/" packaging/pluginst.inf.in > "$STAGE/pluginst.inf"

# Everything sits at the archive root, with no wrapping directory: that is
# where Double Commander and Total Commander look for pluginst.inf when
# deciding whether a zip is an installable plugin. A wrapping directory makes
# the archive open as a plain zip instead.
rm -f "dist/$NAME.zip"
( cd "$STAGE" && zip -q -r "../$NAME.zip" . )
rm -rf "$STAGE"

echo "package.sh: wrote dist/$NAME.zip"
