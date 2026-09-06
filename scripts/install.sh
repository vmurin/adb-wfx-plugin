#!/usr/bin/env bash
# install.sh -- installs fsplugin.wfx64 into Double Commander and registers it.
#
#   ./install.sh                 install and register (the normal case)
#   ./install.sh --no-register   only copy the file; register by hand
#   ./install.sh --print-xml     print the doublecmd.xml block and exit
#   ./install.sh --help
#
# The plugin goes into Double Commander's own configuration directory, under
# plugins/wfx/adb/. Deliberately NOT into the application bundle or /usr/lib: a
# plugin installed there is wiped by the next Double Commander update, and on
# macOS writing inside the .app invalidates its code signature. DC resolves
# plugin paths through %DC_CONFIG_PATH%, so a plugin in the config directory
# works exactly the same and survives upgrades.
#
# Writes ONLY inside .../plugins/wfx/adb/ and, for the registration step, into
# doublecmd.xml -- never into the sibling plugin directories that may live next
# to it. Backs up anything it overwrites first.
#
# The three release archives all contain a file called fsplugin.wfx64, and only
# one of them is native to any given machine. This script refuses to install a
# plugin built for another platform, because Double Commander's only complaint
# about one is "This is not a valid plugin!", said long afterwards and pointing
# at nothing in particular.
#
# Copying the file is only half of an install: Double Commander does not scan
# for plugins, it loads the ones listed in its configuration. That is what the
# registration step is for. It needs python3 and needs Double Commander to be
# closed, because DC rewrites doublecmd.xml when it exits and would discard the
# change. When registration cannot run, the script says so and prints the
# manual steps instead of failing silently.
set -euo pipefail

PLUGIN_FILE="fsplugin.wfx64"
PLUGIN_NAME="ADB"
PRINT_XML=0
REGISTER=1

for arg in "$@"; do
    case "$arg" in
        --print-xml) PRINT_XML=1 ;;
        --no-register) REGISTER=0 ;;
        -h|--help)
            # Everything from line 2 down to the first non-comment line.
            # `\?` is a GNU extension BSD sed does not honour, so the comment
            # marker and its space come off in two plain substitutions.
            sed -n '2,/^[^#]/p' "$0" | sed -e '$d' -e 's/^#//' -e 's/^ //'
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

# register_plugin.py sits next to this script in both layouts: in scripts/ in a
# source checkout, and at the archive root in a release.
REGISTRAR="$HERE/register_plugin.py"

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

# Double Commander can be made to use either location on either OS, and a
# Flatpak or Snap install keeps its configuration inside its own sandbox, so
# prefer whichever candidate actually holds a doublecmd.xml before falling back
# to the platform default. Unmatched globs stay literal and simply fail the
# -f test below, so the sandbox entries cost nothing when DC is not installed
# that way.
CONFIG_DIR=""
for candidate in "$DEFAULT_CONFIG_DIR" \
                 "$HOME/Library/Preferences/doublecmd" \
                 "${XDG_CONFIG_HOME:-$HOME/.config}/doublecmd" \
                 "$HOME"/.var/app/*/config/doublecmd \
                 "$HOME"/snap/*/current/.config/doublecmd; do
    if [ -f "$candidate/doublecmd.xml" ]; then
        CONFIG_DIR="$candidate"
        break
    fi
done

CONFIG_FOUND=1
if [ -z "$CONFIG_DIR" ]; then
    CONFIG_FOUND=0
    CONFIG_DIR="$DEFAULT_CONFIG_DIR"
fi

# The ONLY directory this script may create, write into, or overwrite files in.
INSTALL_DIR="$CONFIG_DIR/plugins/wfx/adb"
TARGET="$INSTALL_DIR/$PLUGIN_FILE"
CONFIG_XML="$CONFIG_DIR/doublecmd.xml"
PATH_VALUE="%DC_CONFIG_PATH%/plugins/wfx/adb/$PLUGIN_FILE"

print_xml_block() {
    cat <<XML
      <WfxPlugin Enabled="True">
        <Name>$PLUGIN_NAME</Name>
        <Path>$PATH_VALUE</Path>
      </WfxPlugin>
XML
}

if [ "$PRINT_XML" -eq 1 ]; then
    print_xml_block
    exit 0
fi

# Look at the file itself before copying it anywhere: every release archive
# holds one called fsplugin.wfx64, and only one of them is native here.
#
# The format -- Mach-O against ELF -- is fatal: no amount of configuration
# makes a Linux library load on macOS. The architecture is only a warning on
# macOS, because an x86_64 build does load into a Double Commander running
# under Rosetta; on Linux there is no such second chance.

# <file> <offset> <count> -> those bytes as lowercase hex, no separators.
file_bytes() {
    od -An -tx1 -j "$2" -N "$3" "$1" | tr -d ' \n'
}

case "$OS" in
    Darwin) WANT_ARCHIVE="adb-wfx-<version>-macos-universal.zip" ;;
    Linux)  WANT_ARCHIVE="adb-wfx-<version>-linux-$(uname -m).zip" ;;
esac

foreign_plugin() { # <what the file actually is>
    echo "install.sh: $SOURCE is $1." >&2
    echo "  This system cannot load it: Double Commander would reject it with" >&2
    echo "  \"This is not a valid plugin!\" once you registered it. Nothing has" >&2
    echo "  been installed. Download the archive built for this platform:" >&2
    echo "    $WANT_ARCHIVE" >&2
    echo "  https://github.com/vmurin/adb-wfx-plugin/releases/latest" >&2
    exit 1
}

MAGIC="$(file_bytes "$SOURCE" 0 4)"
case "$OS" in
    Darwin)
        case "$MAGIC" in
            # A thin 64-bit Mach-O, or a universal ("fat") one holding several.
            cffaedfe|cafebabe|cafebabf) ;;
            7f454c46) foreign_plugin "a Linux library (ELF)" ;;
            *)        foreign_plugin "not a macOS library (no Mach-O header)" ;;
        esac
        # lipo ships with the Xcode command line tools and may not be here; for
        # a thin file the cputype in the header answers the same question.
        ARCHS=""
        if command -v lipo >/dev/null 2>&1; then
            ARCHS="$(lipo -archs "$SOURCE" 2>/dev/null || true)"
        elif [ "$MAGIC" = "cffaedfe" ]; then
            case "$(file_bytes "$SOURCE" 4 4)" in
                07000001) ARCHS="x86_64" ;;
                0c000001) ARCHS="arm64" ;;
            esac
        fi
        if [ -n "$ARCHS" ]; then
            case " $ARCHS " in
                *" $(uname -m) "*) ;;
                *)
                    echo "install.sh: warning: this plugin is built for" \
                         "$ARCHS, and this Mac is $(uname -m)." >&2
                    echo "  It will load only into a Double Commander running" >&2
                    echo "  under Rosetta. The universal build in" >&2
                    echo "  $WANT_ARCHIVE loads into either." >&2
                    ;;
            esac
        fi
        ;;
    Linux)
        case "$MAGIC" in
            7f454c46) ;;
            cffaedfe|cafebabe|cafebabf) foreign_plugin "a macOS library (Mach-O)" ;;
            *) foreign_plugin "not a Linux library (no ELF header)" ;;
        esac
        # e_machine: two little-endian bytes at offset 0x12. Only the two
        # architectures this project ships are named -- anything else falls
        # through rather than being refused on a guess.
        case "$(uname -m):$(file_bytes "$SOURCE" 18 2)" in
            x86_64:b700)             foreign_plugin "an ARM (aarch64) library" ;;
            aarch64:3e00|arm64:3e00) foreign_plugin "an x86-64 library" ;;
        esac
        ;;
esac

# -x matches the process name exactly, unlike -f, which matches the whole
# command line and would happily match this script's own arguments. The
# executable is called doublecmd on macOS too (inside the .app bundle); the
# suffixed names are what some Linux distributions ship.
dc_running() {
    local name
    for name in doublecmd doublecmd-gtk doublecmd-qt; do
        if pgrep -x "$name" >/dev/null 2>&1; then
            return 0
        fi
    done
    return 1
}

# On macOS without the Xcode command line tools, python3 exists as a stub that
# fails when actually invoked -- so run it rather than trusting `command -v`.
have_python3() {
    command -v python3 >/dev/null 2>&1 \
        && python3 -c "import xml.etree.ElementTree" >/dev/null 2>&1
}

manual_instructions() {
    cat <<INSTRUCTIONS

Register it by hand instead -- Double Commander does not pick up plugins on
its own, so until this is done nothing will change:

  1. Configuration -> Options... -> Plugins -> WFX plugins
  2. Click "Add", select:
       $TARGET
  3. Name it  $PLUGIN_NAME  and confirm with OK, then Apply.
  4. Quit Double Commander completely and start it again.

Or, with Double Commander closed, add this inside <WfxPlugins> in
$CONFIG_XML:

$(print_xml_block)
INSTRUCTIONS
}

if [ "$CONFIG_FOUND" -eq 0 ]; then
    echo "install.sh: no doublecmd.xml found -- assuming $CONFIG_DIR."
    echo "  If Double Commander has never been started, start it once and quit,"
    echo "  then re-run this script so it lands next to the real configuration."
    echo
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

# The file is in place. Everything below is the registration half.

if [ "$REGISTER" -eq 0 ]; then
    echo "install.sh: --no-register given, leaving doublecmd.xml alone."
    manual_instructions
    exit 0
fi

if [ "$CONFIG_FOUND" -eq 0 ]; then
    echo "install.sh: cannot register -- $CONFIG_XML does not exist."
    echo "  Start Double Commander once, quit it, and re-run this script."
    manual_instructions
    exit 0
fi

if ! command -v pgrep >/dev/null 2>&1; then
    echo "install.sh: no pgrep here, cannot check whether Double Commander is"
    echo "  running. If it is, quit it and re-run this script -- DC rewrites"
    echo "  doublecmd.xml on exit and would discard the registration."
elif dc_running; then
    echo "install.sh: Double Commander is running, so the registration was" >&2
    echo "  skipped: DC rewrites doublecmd.xml when it exits and would discard" >&2
    echo "  the change. Quit it completely and run this script again." >&2
    echo "  (The plugin file itself is already installed.)" >&2
    exit 1
fi

if [ ! -f "$REGISTRAR" ]; then
    echo "install.sh: register_plugin.py not found next to this script."
    manual_instructions
    exit 0
fi

if ! have_python3; then
    echo "install.sh: no working python3 found, so the registration was skipped."
    echo "  On macOS, python3 comes with the Xcode command line tools:"
    echo "    xcode-select --install"
    manual_instructions
    exit 0
fi

echo "install.sh: registering in $CONFIG_XML"
if ! python3 "$REGISTRAR" "$CONFIG_XML" "$PATH_VALUE"; then
    echo "install.sh: registration failed -- doublecmd.xml was left alone." >&2
    manual_instructions
    exit 0
fi

cat <<DONE

Done. The plugin is installed and registered as "$PLUGIN_NAME".

  1. Start Double Commander (if it was open, quit it completely first).
  2. Commands -> Open VFS list, and choose $PLUGIN_NAME.
  3. On the phone: enable USB debugging and authorise this computer when
     prompted.

Devices then appear as directories named <model> (<serial>).
DONE
