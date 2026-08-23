#!/usr/bin/env bash
# scripts/install.sh -- installs the built fsplugin.wfx64 into Double
# Commander's wfx plugin directory, under its own "adb" subdirectory.
#
# Writes ONLY inside .../plugins/wfx/adb/ -- never touches the sibling
# plugin directories (e.g. ftp/, mtp/, MacCloud/) that live next to it.
# Backs up any existing fsplugin.wfx64 there to
# fsplugin.wfx64.bak-<timestamp> before overwriting.
set -euo pipefail

cd "$(dirname "$0")/.."

PLUGIN_FILE="fsplugin.wfx64"
DC_APP="/Applications/Double Commander.app"
WFX_PLUGINS_DIR="$DC_APP/Contents/MacOS/plugins/wfx"
# The ONLY directory this script may create, write into, or overwrite
# files in. Never remove or modify anything outside it (in particular,
# never touch $WFX_PLUGINS_DIR's other subdirectories).
INSTALL_DIR="$WFX_PLUGINS_DIR/adb"

if [ ! -f "$PLUGIN_FILE" ]; then
    echo "install.sh: $PLUGIN_FILE not found in $(pwd) -- run ./compile_mac.sh first." >&2
    exit 1
fi

if [ ! -d "$DC_APP" ]; then
    echo "install.sh: Double Commander not found at $DC_APP" >&2
    exit 1
fi

mkdir -p "$INSTALL_DIR"

TARGET="$INSTALL_DIR/$PLUGIN_FILE"
if [ -f "$TARGET" ]; then
    TIMESTAMP="$(date +%Y%m%d%H%M%S)"
    BACKUP="$TARGET.bak-$TIMESTAMP"
    cp "$TARGET" "$BACKUP"
    echo "install.sh: backed up existing plugin to $BACKUP"
fi

cp "$PLUGIN_FILE" "$TARGET"
echo "install.sh: installed $TARGET"
echo
echo "Next steps:"
echo "  1. Quit Double Commander completely and relaunch it."
echo "  2. If the ADB filesystem plugin does not appear automatically,"
echo "     open Configuration -> Plugins -> WFX plugins, click Add/Configure,"
echo "     and point it at:"
echo "       $TARGET"
