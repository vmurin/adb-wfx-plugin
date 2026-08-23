#!/usr/bin/env bash
# Asserts that fsplugin.wfx64 exports every symbol Double Commander needs --
# and nothing else. Builds the plugin first if it isn't there yet, so this
# also works on a clean checkout.
#
# The symbol table is read with the platform's own tool: `nm -gU` on Mach-O,
# where every C-linkage symbol carries a leading underscore, and
# `nm -D --defined-only` on ELF, where it does not.
set -euo pipefail

cd "$(dirname "$0")/.."

if [ ! -f fsplugin.wfx64 ]; then
    ./build.sh
fi

EXPORTS=(
    FsInitW
    FsFindFirstW
    FsFindNextW
    FsFindClose
    FsGetFileW
    FsPutFileW
    FsRenMovFileW
    FsDeleteFileW
    FsRemoveDirW
    FsMkDirW
    FsSetTimeW
    FsStatusInfoW
    FsGetDefRootName
    FsDisconnectW
)

case "$(uname -s)" in
    Darwin)
        actual="$(nm -gU fsplugin.wfx64 | awk '{print $NF}')"
        prefix="_"
        ;;
    Linux)
        actual="$(nm -D --defined-only fsplugin.wfx64 | awk '{print $NF}')"
        prefix=""
        ;;
    *)
        echo "check-exports.sh: unsupported platform: $(uname -s)" >&2
        exit 2
        ;;
esac

missing=""
for symbol in "${EXPORTS[@]}"; do
    if ! grep -qx "${prefix}${symbol}" <<< "$actual"; then
        missing="${missing}  ${symbol}"$'\n'
    fi
done

if [ -n "$missing" ]; then
    echo "check-exports.sh FAILED: fsplugin.wfx64 is missing these exports:" >&2
    printf '%s' "$missing" >&2
    exit 1
fi

# FsSetTimeW is non-negotiable, called out here by name rather than only
# swept up in the loop above: Double Commander gates copying file dates,
# in BOTH directions, on Assigned(FsSetTime) or Assigned(FsSetTimeW), and
# silently strips caoCopyTime from every copy operation when neither is
# exported -- losing dates even on downloads, where this plugin plays no
# part in setting them.
if ! grep -qx "${prefix}FsSetTimeW" <<< "$actual"; then
    echo "check-exports.sh FAILED: FsSetTimeW is not exported." >&2
    echo "Double Commander will silently stop copying file dates in both directions." >&2
    exit 1
fi

# Nothing from the C++ side may leak out alongside them. -fvisibility=hidden
# (and, on ELF, fsplugin.map) is what keeps that true; this catches the day
# someone adds a stray non-static definition or an explicit dllexport.
leaked="$(grep '^_\?_Z' <<< "$actual" || true)"
if [ -n "$leaked" ]; then
    echo "check-exports.sh FAILED: mangled C++ symbols are exported:" >&2
    printf '%s\n' "$leaked" >&2
    exit 1
fi

echo "check-exports.sh: all exports present (FsSetTimeW confirmed by name), no C++ symbols leaked."
