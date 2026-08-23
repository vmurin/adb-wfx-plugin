#!/usr/bin/env bash
# Asserts, via `nm -gU`, that fsplugin.wfx64 exports every symbol Double
# Commander needs. Builds the plugin
# first if it isn't there yet, so this also works on a clean checkout.
set -euo pipefail

cd "$(dirname "$0")/.."

if [ ! -f fsplugin.wfx64 ]; then
    ./compile_mac.sh
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

actual="$(nm -gU fsplugin.wfx64 | awk '{print $NF}')"

missing=""
for symbol in "${EXPORTS[@]}"; do
    # Mach-O gives every C-linkage symbol a leading underscore.
    if ! grep -qx "_${symbol}" <<< "$actual"; then
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
if ! grep -qx "_FsSetTimeW" <<< "$actual"; then
    echo "check-exports.sh FAILED: FsSetTimeW is not exported." >&2
    echo "Double Commander will silently stop copying file dates in both directions." >&2
    exit 1
fi

echo "check-exports.sh: all exports present (FsSetTimeW confirmed by name)."
