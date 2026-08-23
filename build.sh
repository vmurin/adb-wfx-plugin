#!/usr/bin/env bash
# build.sh -- the one place that knows how to compile this plugin.
#
#   ./build.sh              native build          -> fsplugin.wfx64
#   ./build.sh --universal  macOS arm64+x86_64    -> fsplugin.wfx64
#   CXX=g++ ./build.sh      pick the compiler
#
# compile_mac.sh and compile_mac_universal.sh are thin wrappers around this,
# kept so older instructions and muscle memory still work.
set -euo pipefail

cd "$(dirname "$0")"

[ -f fsplugin.cpp ] || { echo "build.sh: fsplugin.cpp not present"; exit 1; }

UNIVERSAL=0
for arg in "$@"; do
    case "$arg" in
        --universal) UNIVERSAL=1 ;;
        *) echo "build.sh: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

OS="$(uname -s)"

# Default compiler per platform, overridable through $CXX so CI and distro
# packagers can pick their own.
if [ -z "${CXX:-}" ]; then
    case "$OS" in
        Darwin) CXX=clang++ ;;
        *)      CXX=g++ ;;
    esac
fi

# -fvisibility=hidden plus the explicit WFX_EXPORT markers in fsplugin.cpp is
# what keeps the exported symbol table down to the 14 names Double Commander
# asks for; scripts/check-exports.sh asserts the result.
COMMON_FLAGS=(-std=c++17 -Wall -Wextra -Werror -O2 -fPIC -fvisibility=hidden -I.)
LINK_FLAGS=(-shared)

case "$OS" in
    Darwin)
        if [ "$UNIVERSAL" -eq 1 ]; then
            COMMON_FLAGS+=(-arch arm64 -arch x86_64)
        fi
        # Keep the Mach-O export table to the plugin's own Fs* entry points.
        # See fsplugin.exp: -fvisibility=hidden leaves weak RTTI symbols
        # exported, which is visible on x86_64 though not on arm64.
        LINK_FLAGS+=(-Wl,-exported_symbols_list,fsplugin.exp)
        ;;
    Linux)
        if [ "$UNIVERSAL" -eq 1 ]; then
            echo "build.sh: --universal is macOS-only" >&2
            exit 2
        fi
        # Double Commander is a Free Pascal program and imposes no C++ runtime,
        # so linking libstdc++ statically costs nothing here and spares users
        # the "built on Ubuntu 24.04, will not load on Debian 12" class of bug.
        LINK_FLAGS+=(-static-libstdc++ -static-libgcc)
        # Keep the ELF dynamic symbol table to the plugin's own Fs* exports.
        # -fvisibility=hidden alone still leaves the linker's _init/_fini/_end
        # in .dynsym, which would weaken check-exports.sh's "exactly these"
        # property.
        LINK_FLAGS+=(-Wl,--version-script=fsplugin.map)
        ;;
    *)
        echo "build.sh: unsupported platform: $OS" >&2
        echo "  Only macOS and Linux are supported. Windows (and with it Total" >&2
        echo "  Commander) is tracked separately -- see the README." >&2
        exit 2
        ;;
esac

"$CXX" "${COMMON_FLAGS[@]}" "${LINK_FLAGS[@]}" fsplugin.cpp -o fsplugin.wfx64

echo "build.sh: built fsplugin.wfx64 ($OS, $CXX)"
