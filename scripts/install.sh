#!/usr/bin/env bash
# install.sh -- copies fsplugin.wfx64 into Double Commander's own configuration
# directory, under plugins/wfx/adb/.
#
# Deliberately NOT into the application bundle or /usr/lib: a plugin installed
# there is wiped by the next Double Commander update, and on macOS writing
# inside the .app invalidates its code signature. DC resolves plugin paths
# through %DC_CONFIG_PATH%, so a plugin in the config directory works exactly
# the same and survives upgrades.
#
# Writes ONLY inside .../plugins/wfx/adb/ -- never touches the sibling plugin
# directories that may live next to it. Backs up any existing fsplugin.wfx64
# to fsplugin.wfx64.bak-<timestamp> before overwriting.
#
# Copying the file is only half of an install. Double Commander does not scan
# for plugins; it loads the ones listed in its configuration. The registration
# steps this script prints at the end are required, not optional.
set -euo pipefail

PLUGIN_FILE="fsplugin.wfx64"
PRINT_XML=0

for arg in "$@"; do
    case "$arg" in
        --print-xml) PRINT_XML=1 ;;
        -h|--help)
            sed -n '2,20p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "install.sh: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

# Run either from a source checkout (scripts/install.sh, plugin one level up)
# or from an unpacked release archive (install.sh next to the plugin).
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$HERE/$PLUGIN_FILE" ]; then
    SOURCE="$HERE/$PLUGIN_FILE"
elif [ -f "$HERE/../$PLUGIN_FILE" ]; then
    SOURCE="$HERE/../$PLUGIN_FILE"
else
    echo "install.sh: $PLUGIN_FILE not found." >&2
    echo "  Build it first with ./build.sh, or run this script from inside the" >&2
    echo "  unpacked release archive." >&2
    exit 1
fi

OS="$(uname -s)"
case "$OS" in
    Darwin) DEFAULT_CONFIG_DIR="$HOME/Library/Preferences/doublecmd" ;;
    Linux)  DEFAULT_CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/doublecmd" ;;
    *)
        echo "install.sh: unsupported platform: $OS" >&2
        echo "  Only macOS and Linux are supported." >&2
        exit 2
        ;;
esac

# Double Commander can be made to use either location on either OS, so prefer
# whichever one actually exists before falling back to the platform default.
CONFIG_DIR=""
for candidate in "$DEFAULT_CONFIG_DIR" \
                 "$HOME/Library/Preferences/doublecmd" \
                 "${XDG_CONFIG_HOME:-$HOME/.config}/doublecmd"; do
    if [ -f "$candidate/doublecmd.xml" ]; then
        CONFIG_DIR="$candidate"
        break
    fi
done

if [ -z "$CONFIG_DIR" ]; then
    CONFIG_DIR="$DEFAULT_CONFIG_DIR"
    echo "install.sh: no doublecmd.xml found -- assuming $CONFIG_DIR."
    echo "  If Double Commander has never been started, start it once and quit,"
    echo "  then re-run this script so it lands next to the real configuration."
    echo
fi

# The ONLY directory this script may create, write into, or overwrite files in.
INSTALL_DIR="$CONFIG_DIR/plugins/wfx/adb"
TARGET="$INSTALL_DIR/$PLUGIN_FILE"

if [ "$PRINT_XML" -eq 1 ]; then
    cat <<XML
      <WfxPlugin Enabled="True">
        <Name>ADB</Name>
        <Path>%DC_CONFIG_PATH%/plugins/wfx/adb/$PLUGIN_FILE</Path>
      </WfxPlugin>
XML
    exit 0
fi

mkdir -p "$INSTALL_DIR"

if [ -f "$TARGET" ]; then
    BACKUP="$TARGET.bak-$(date +%Y%m%d%H%M%S)"
    cp "$TARGET" "$BACKUP"
    echo "install.sh: backed up existing plugin to $BACKUP"
fi

cp "$SOURCE" "$TARGET"
chmod 755 "$TARGET"

# A plugin downloaded from a release carries com.apple.quarantine, and macOS
# refuses to load a quarantined library into Double Commander. Harmless on a
# locally built file, which has no such attribute.
if [ "$OS" = "Darwin" ] && command -v xattr >/dev/null 2>&1; then
    xattr -d com.apple.quarantine "$TARGET" 2>/dev/null || true
fi

echo "install.sh: installed $TARGET"
cat <<INSTRUCTIONS

The file is in place. Double Commander does not pick up plugins on its own --
you must now register it, or nothing will change:

  1. Configuration -> Options... -> Plugins -> WFX plugins
  2. Click "Add", select:
       $TARGET
  3. Name it  ADB  and confirm with OK, then Apply.
  4. Quit Double Commander completely and start it again.

Then open the plugin: Commands -> Open VFS list, and choose ADB.

To confirm the registration landed, look for this in
$CONFIG_DIR/doublecmd.xml:

      <WfxPlugin Enabled="True">
        <Name>ADB</Name>
        <Path>%DC_CONFIG_PATH%/plugins/wfx/adb/$PLUGIN_FILE</Path>
      </WfxPlugin>

Finally, on the phone: enable USB debugging and authorise this computer when
prompted.
INSTRUCTIONS
