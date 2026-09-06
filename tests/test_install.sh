#!/usr/bin/env bash
# tests/test_install.sh -- covers scripts/register_plugin.py and the
# registration half of scripts/install.sh. Needs no Android device and no
# Double Commander; everything happens inside one mktemp -d sandbox.
#
# SAFETY: this suite must never touch the real Double Commander configuration.
# It never runs install.sh without overriding HOME and XDG_CONFIG_HOME to point
# inside the sandbox, and it shadows pgrep with a stub so a Double Commander
# that happens to be running on the developer's machine cannot change the
# result either way.
set -euo pipefail

cd "$(dirname "$0")/.."
REPO="$PWD"

if ! command -v python3 >/dev/null 2>&1 \
   || ! python3 -c "import xml.etree.ElementTree" >/dev/null 2>&1; then
    echo "test_install.sh: SKIPPED -- no working python3 on this machine."
    exit 0
fi

SANDBOX="$(mktemp -d)"
trap 'rm -rf "$SANDBOX"' EXIT

CHECKS=0
FAILURES=0

pass() { CHECKS=$((CHECKS + 1)); }
fail() {
    CHECKS=$((CHECKS + 1))
    FAILURES=$((FAILURES + 1))
    echo "  FAIL: $*" >&2
}
assert_eq() { # desc expected actual
    if [ "$2" = "$3" ]; then pass; else fail "$1: expected [$2], got [$3]"; fi
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

REGISTRAR="$REPO/scripts/register_plugin.py"
WANT_PATH="%DC_CONFIG_PATH%/plugins/wfx/adb/fsplugin.wfx64"

# Every <WfxPlugin> as "Name|Path", one per line, in document order. Reading
# the result back with a parser rather than grep is what makes the ordering and
# nesting assertions below mean anything.
dump_wfx() {
    python3 - "$1" <<'PY'
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
plugins = root.find("Plugins")
wfx = plugins.find("WfxPlugins") if plugins is not None else None
for el in (list(wfx) if wfx is not None else []):
    print("%s|%s" % (el.findtext("Name") or "", el.findtext("Path") or ""))
PY
}

# --- fixtures ---------------------------------------------------------------

fixture_no_plugins() {
    cat >"$1" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<doublecmd DCVersion="1.1.28" ConfigVersion="15">
  <Language>
    <POFileName>english.po</POFileName>
  </Language>
</doublecmd>
XML
}

fixture_no_wfx_section() {
    cat >"$1" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<doublecmd DCVersion="1.1.28" ConfigVersion="15">
  <Plugins Version="3">
    <DsxPlugins/>
  </Plugins>
</doublecmd>
XML
}

# A copy of the shape a real doublecmd.xml has, including the detail that
# matters most here: DC's own MTP plugin is ALSO called fsplugin.wfx64.
fixture_populated() {
    cat >"$1" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<doublecmd DCVersion="1.1.28" ConfigVersion="15">
  <Plugins Version="3">
    <DsxPlugins/>
    <WfxPlugins>
      <WfxPlugin Enabled="True">
        <Name>FTP</Name>
        <Path>%commander_path%/plugins/wfx/ftp/ftp.wfx</Path>
      </WfxPlugin>
      <WfxPlugin Enabled="True">
        <Name>MTP</Name>
        <Path>%commander_path%/plugins/wfx/mtp/fsplugin.wfx64</Path>
      </WfxPlugin>
      <WfxPlugin Enabled="True">
        <Name>cloud</Name>
        <Path>%commander_path%/plugins/wfx/MacCloud/MacCloud.wfx</Path>
      </WfxPlugin>
    </WfxPlugins>
  </Plugins>
</doublecmd>
XML
}

# The same, but with an ADB entry already registered from inside the app
# bundle -- what someone who followed the old manual instructions would have.
fixture_stale_adb() {
    cat >"$1" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<doublecmd DCVersion="1.1.28" ConfigVersion="15">
  <Plugins Version="3">
    <WfxPlugins>
      <WfxPlugin Enabled="True">
        <Name>MTP</Name>
        <Path>%commander_path%/plugins/wfx/mtp/fsplugin.wfx64</Path>
      </WfxPlugin>
      <WfxPlugin Enabled="True">
        <Name>ADB</Name>
        <Path>%commander_path%/plugins/wfx/adb/fsplugin.wfx64</Path>
      </WfxPlugin>
    </WfxPlugins>
  </Plugins>
</doublecmd>
XML
}

# --- register_plugin.py -----------------------------------------------------

echo "test_install.sh: register_plugin.py"

# 1. A configuration with no <Plugins> section at all.
CFG="$SANDBOX/case1.xml"
fixture_no_plugins "$CFG"
python3 "$REGISTRAR" "$CFG" "$WANT_PATH" >/dev/null
assert_eq "1: sole entry is ours" "ADB|$WANT_PATH" "$(dump_wfx "$CFG")"
assert_contains "1: untouched siblings survive" "$(cat "$CFG")" "<POFileName>english.po</POFileName>"

# 2. <Plugins> present but no <WfxPlugins>; the Version attribute must survive.
CFG="$SANDBOX/case2.xml"
fixture_no_wfx_section "$CFG"
python3 "$REGISTRAR" "$CFG" "$WANT_PATH" >/dev/null
assert_eq "2: sole entry is ours" "ADB|$WANT_PATH" "$(dump_wfx "$CFG")"
assert_contains "2: Plugins Version kept" "$(cat "$CFG")" '<Plugins Version="3">'
assert_contains "2: DsxPlugins kept" "$(cat "$CFG")" "<DsxPlugins/>"

# 3. A populated section: we are appended and nothing else is disturbed. The
#    MTP entry is the regression guard -- it shares our file name, so matching
#    on the basename rather than on the name or the full path would rewrite
#    someone's MTP registration.
CFG="$SANDBOX/case3.xml"
fixture_populated "$CFG"
python3 "$REGISTRAR" "$CFG" "$WANT_PATH" >/dev/null
DUMP="$(dump_wfx "$CFG")"
assert_eq "3: four entries" "4" "$(printf '%s\n' "$DUMP" | wc -l | tr -d ' ')"
assert_contains "3: MTP untouched" "$DUMP" "MTP|%commander_path%/plugins/wfx/mtp/fsplugin.wfx64"
assert_contains "3: FTP untouched" "$DUMP" "FTP|%commander_path%/plugins/wfx/ftp/ftp.wfx"
assert_contains "3: ours added" "$DUMP" "ADB|$WANT_PATH"

# 4. Running it again is a no-op, not a second entry.
python3 "$REGISTRAR" "$CFG" "$WANT_PATH" >/dev/null
DUMP="$(dump_wfx "$CFG")"
assert_eq "4: still four entries" "4" "$(printf '%s\n' "$DUMP" | wc -l | tr -d ' ')"
assert_eq "4: exactly one ADB" "1" "$(printf '%s\n' "$DUMP" | grep -c '^ADB|')"

# 5. A stale ADB entry pointing into the app bundle is updated in place.
CFG="$SANDBOX/case5.xml"
fixture_stale_adb "$CFG"
python3 "$REGISTRAR" "$CFG" "$WANT_PATH" >/dev/null
DUMP="$(dump_wfx "$CFG")"
assert_eq "5: two entries" "2" "$(printf '%s\n' "$DUMP" | wc -l | tr -d ' ')"
assert_contains "5: path rewritten" "$DUMP" "ADB|$WANT_PATH"
assert_missing "5: old path gone" "$DUMP" "ADB|%commander_path%"

# 6. A configuration we cannot parse is refused, left byte-for-byte alone, and
#    does not even get a backup -- there is nothing worth preserving and a
#    stray .bak of a broken file only confuses.
CFG="$SANDBOX/case6.xml"
printf '<?xml version="1.0"?>\n<doublecmd>\n  <Plugins>\n' >"$CFG"
cp "$CFG" "$SANDBOX/case6.orig"
if python3 "$REGISTRAR" "$CFG" "$WANT_PATH" >/dev/null 2>&1; then
    fail "6: malformed XML should be refused"
else
    pass
fi
if cmp -s "$CFG" "$SANDBOX/case6.orig"; then pass; else fail "6: malformed config was modified"; fi
assert_eq "6: no backup left behind" "0" "$(find "$SANDBOX" -name 'case6.xml.bak-*' | wc -l | tr -d ' ')"

# 7. A successful run backs the original up first.
CFG="$SANDBOX/case7.xml"
fixture_populated "$CFG"
cp "$CFG" "$SANDBOX/case7.orig"
python3 "$REGISTRAR" "$CFG" "$WANT_PATH" >/dev/null
BAK="$(find "$SANDBOX" -name 'case7.xml.bak-*' | head -1)"
if [ -n "$BAK" ] && cmp -s "$BAK" "$SANDBOX/case7.orig"; then
    pass
else
    fail "7: backup missing or does not match the original"
fi

# --- install.sh -------------------------------------------------------------

echo "test_install.sh: install.sh"

# A stand-in for the plugin keeps this suite independent of whether the real
# one has been built yet -- but install.sh reads the file's header now, so the
# stand-in has to carry a real one. Only the header is real: nothing here is
# ever loaded, only inspected.
#
#   native      what this machine can load
#   foreign     the other operating system's format -- i.e. the wrong archive
#   wrong-arch  the right format, the other architecture
make_plugin() { # <path> <kind>
    python3 - "$1" "$2" "$(uname -s)" "$(uname -m)" <<'MAKEPLUGIN'
import struct
import sys

path, kind, osname, machine = sys.argv[1:5]

CPU_X86_64, CPU_ARM64 = 0x01000007, 0x0100000C
EM_X86_64, EM_AARCH64 = 62, 183


def macho(cputype):
    # MH_MAGIC_64, cputype, cpusubtype, MH_DYLIB and an empty load-command
    # table, padded so the header is not the whole file.
    return struct.pack("<IiiIIII", 0xFEEDFACF, cputype, 0, 6, 0, 0, 0) + b"\0" * 32


def elf(e_machine):
    h = bytearray(64)
    h[0:4] = b"\x7fELF"
    h[4] = 2  # ELFCLASS64
    h[5] = 1  # ELFDATA2LSB
    h[6] = 1  # EV_CURRENT
    h[16:18] = struct.pack("<H", 3)  # ET_DYN
    h[18:20] = struct.pack("<H", e_machine)
    return bytes(h)


if osname == "Darwin":
    host, other = (CPU_ARM64, CPU_X86_64) if machine == "arm64" \
        else (CPU_X86_64, CPU_ARM64)
    data = {
        "native": lambda: macho(host),
        "wrong-arch": lambda: macho(other),
        "foreign": lambda: elf(EM_AARCH64),
    }[kind]()
else:
    host, other = (EM_AARCH64, EM_X86_64) if machine in ("aarch64", "arm64") \
        else (EM_X86_64, EM_AARCH64)
    data = {
        "native": lambda: elf(host),
        "wrong-arch": lambda: elf(other),
        "foreign": lambda: macho(CPU_ARM64),
    }[kind]()

with open(path, "wb") as fh:
    fh.write(data)
MAKEPLUGIN
}

# A release-archive layout: install.sh, the registrar and the plugin side by
# side.
new_release() { # <name> <kind> -> echoes the release directory
    local dir="$SANDBOX/$1"
    mkdir -p "$dir"
    cp "$REPO/scripts/install.sh" "$REPO/scripts/register_plugin.py" "$dir/"
    make_plugin "$dir/fsplugin.wfx64" "$2"
    echo "$dir"
}

RELEASE="$(new_release release native)"

# Stubs that come first on PATH. pgrep-absent is the honest default for these
# tests: it makes "is Double Commander running" answer no, deterministically.
STUBS="$SANDBOX/stubs"
mkdir -p "$STUBS"
printf '#!/bin/sh\nexit 1\n' >"$STUBS/pgrep"
chmod 755 "$STUBS/pgrep"

# A fresh fake home with a doublecmd.xml where this platform's install.sh
# looks for it.
new_home() { # <name> -> echoes the config directory
    local home="$SANDBOX/$1" cfgdir
    case "$(uname -s)" in
        Darwin) cfgdir="$home/Library/Preferences/doublecmd" ;;
        *)      cfgdir="$home/.config/doublecmd" ;;
    esac
    mkdir -p "$cfgdir"
    fixture_populated "$cfgdir/doublecmd.xml"
    echo "$cfgdir"
}

run_install() { # <home-name> [args...]
    local home="$SANDBOX/$1"
    shift
    env HOME="$home" XDG_CONFIG_HOME="$home/.config" PATH="$STUBS:$PATH" \
        "$RELEASE/install.sh" "$@"
}

# 8. --print-xml prints the block and writes nothing at all.
HOME8="$SANDBOX/home8"
mkdir -p "$HOME8"
OUT="$(run_install home8 --print-xml)"
assert_contains "8: prints our name" "$OUT" "<Name>ADB</Name>"
assert_contains "8: prints our path" "$OUT" "<Path>$WANT_PATH</Path>"
assert_eq "8: wrote nothing" "0" "$(find "$HOME8" -type f | wc -l | tr -d ' ')"

# 9. --no-register installs the file and leaves the configuration alone.
CFGDIR="$(new_home home9)"
cp "$CFGDIR/doublecmd.xml" "$SANDBOX/home9.orig"
OUT="$(run_install home9 --no-register)"
if [ -x "$CFGDIR/plugins/wfx/adb/fsplugin.wfx64" ]; then pass; else fail "9: plugin not installed"; fi
if cmp -s "$CFGDIR/doublecmd.xml" "$SANDBOX/home9.orig"; then pass; else fail "9: doublecmd.xml was modified"; fi
assert_contains "9: says how to do it by hand" "$OUT" "Register it by hand"

# 10. The normal run: file installed AND registered.
CFGDIR="$(new_home home10)"
run_install home10 >/dev/null
if [ -x "$CFGDIR/plugins/wfx/adb/fsplugin.wfx64" ]; then pass; else fail "10: plugin not installed"; fi
DUMP="$(dump_wfx "$CFGDIR/doublecmd.xml")"
assert_contains "10: registered" "$DUMP" "ADB|$WANT_PATH"
assert_contains "10: MTP untouched" "$DUMP" "MTP|%commander_path%/plugins/wfx/mtp/fsplugin.wfx64"

# 11. With Double Commander running, the file is still installed but the
#     configuration is left alone and the script fails loudly -- DC rewrites
#     doublecmd.xml on exit and would silently discard the registration.
RUNNING="$SANDBOX/stubs-running"
mkdir -p "$RUNNING"
printf '#!/bin/sh\nexit 0\n' >"$RUNNING/pgrep"
chmod 755 "$RUNNING/pgrep"
CFGDIR="$(new_home home11)"
cp "$CFGDIR/doublecmd.xml" "$SANDBOX/home11.orig"
set +e
OUT="$(env HOME="$SANDBOX/home11" XDG_CONFIG_HOME="$SANDBOX/home11/.config" \
        PATH="$RUNNING:$PATH" "$RELEASE/install.sh" 2>&1)"
STATUS=$?
set -e
assert_eq "11: exits nonzero" "1" "$STATUS"
assert_contains "11: says why" "$OUT" "Double Commander is running"
if [ -x "$CFGDIR/plugins/wfx/adb/fsplugin.wfx64" ]; then pass; else fail "11: plugin not installed"; fi
if cmp -s "$CFGDIR/doublecmd.xml" "$SANDBOX/home11.orig"; then pass; else fail "11: doublecmd.xml was modified"; fi

# 12. Without a usable python3 the install still succeeds; it just falls back
#     to telling the user what to do by hand.
NOPY="$SANDBOX/stubs-nopython"
mkdir -p "$NOPY"
cp "$STUBS/pgrep" "$NOPY/pgrep"
printf '#!/bin/sh\nexit 1\n' >"$NOPY/python3"
chmod 755 "$NOPY/python3"
CFGDIR="$(new_home home12)"
cp "$CFGDIR/doublecmd.xml" "$SANDBOX/home12.orig"
OUT="$(env HOME="$SANDBOX/home12" XDG_CONFIG_HOME="$SANDBOX/home12/.config" \
        PATH="$NOPY:$PATH" "$RELEASE/install.sh" 2>&1)"
if [ -x "$CFGDIR/plugins/wfx/adb/fsplugin.wfx64" ]; then pass; else fail "12: plugin not installed"; fi
if cmp -s "$CFGDIR/doublecmd.xml" "$SANDBOX/home12.orig"; then pass; else fail "12: doublecmd.xml was modified"; fi
assert_contains "12: says python3 is missing" "$OUT" "no working python3"
assert_contains "12: prints the XML block" "$OUT" "<Path>$WANT_PATH</Path>"

# 13. A home where Double Commander has never run: no doublecmd.xml to edit,
#     so say so rather than inventing a configuration file.
mkdir -p "$SANDBOX/home13"
OUT="$(run_install home13 2>&1)"
assert_contains "13: explains the missing config" "$OUT" "Start Double Commander once"
assert_eq "13: created no doublecmd.xml" "0" \
    "$(find "$SANDBOX/home13" -name doublecmd.xml | wc -l | tr -d ' ')"

# 14. The wrong release archive: a library for the other operating system.
#     Refuse it here, while the download is still in sight, rather than leaving
#     Double Commander to say "This is not a valid plugin!" much later.
FOREIGN="$(new_release release-foreign foreign)"
CFGDIR="$(new_home home14)"
cp "$CFGDIR/doublecmd.xml" "$SANDBOX/home14.orig"
set +e
OUT="$(env HOME="$SANDBOX/home14" XDG_CONFIG_HOME="$SANDBOX/home14/.config" \
        PATH="$STUBS:$PATH" "$FOREIGN/install.sh" 2>&1)"
STATUS=$?
set -e
assert_eq "14: exits nonzero" "1" "$STATUS"
assert_contains "14: names the symptom" "$OUT" "This is not a valid plugin!"
assert_contains "14: names the right archive" "$OUT" "adb-wfx-<version>-"
if [ -e "$CFGDIR/plugins/wfx/adb/fsplugin.wfx64" ]; then
    fail "14: foreign plugin was installed anyway"
else
    pass
fi
if cmp -s "$CFGDIR/doublecmd.xml" "$SANDBOX/home14.orig"; then pass; else fail "14: doublecmd.xml was modified"; fi

# 15. The right format, the other architecture. On macOS that still loads into
#     a Double Commander running under Rosetta, so it warns and installs; on
#     Linux nothing can load it, so it is refused like case 14.
OTHERARCH="$(new_release release-otherarch wrong-arch)"
CFGDIR="$(new_home home15)"
set +e
OUT="$(env HOME="$SANDBOX/home15" XDG_CONFIG_HOME="$SANDBOX/home15/.config" \
        PATH="$STUBS:$PATH" "$OTHERARCH/install.sh" 2>&1)"
STATUS=$?
set -e
if [ "$(uname -s)" = "Darwin" ]; then
    assert_eq "15: macOS installs it anyway" "0" "$STATUS"
    assert_contains "15: warns about Rosetta" "$OUT" "under Rosetta"
    if [ -x "$CFGDIR/plugins/wfx/adb/fsplugin.wfx64" ]; then pass; else fail "15: plugin not installed"; fi
else
    assert_eq "15: Linux refuses it" "1" "$STATUS"
    assert_contains "15: names the symptom" "$OUT" "This is not a valid plugin!"
    if [ -e "$CFGDIR/plugins/wfx/adb/fsplugin.wfx64" ]; then
        fail "15: wrong-architecture plugin was installed anyway"
    else
        pass
    fi
fi

# --- result -----------------------------------------------------------------

if [ "$FAILURES" -gt 0 ]; then
    echo "test_install.sh: $FAILURES of $CHECKS checks FAILED" >&2
    exit 1
fi
echo "test_install.sh: $CHECKS checks passed"
