#!/usr/bin/env bash
# tests/device_test.sh -- opt-in end-to-end test against a real attached
# Android device. Exercises the built plugin through tests/device_driver.cpp
# (i.e. through PluginCore's own code paths), not through the Double
# Commander GUI, and never through `adb` for the operations under test --
# `adb` is used here only for independent verification (stat, shasum),
# exactly as a human reviewer would.
#
# Run only when ADB_WFX_DEVICE_TESTS=1. Every on-device path this script
# touches is $TEST_ROOT ("/sdcard/adb_wfx_test", tests/device_lib.sh) or a
# path under it -- see assert_under_test_root() there. This script creates
# that directory and removes it (only it) at the end.
#
# Exit codes: 0 all steps passed. $EXIT_NO_DEVICE (42) no device is
# attached -- distinct from a real failure. 1 a step's assertion failed.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tests/device_lib.sh
. tests/device_lib.sh

if [ "${ADB_WFX_DEVICE_TESTS:-0}" != "1" ]; then
    echo "device_test.sh: skipped (set ADB_WFX_DEVICE_TESTS=1 to run)."
    exit 0
fi

require_device
build_driver

WFX_ROOT="/$SERIAL$TEST_ROOT"

LOCAL_SCRATCH="$(mktemp -d)"
cleanup_local() {
    rm -rf "$LOCAL_SCRATCH"
}
trap cleanup_local EXIT

pass() {
    echo "PASS: $1"
}

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

# adb shell's stdout, trimmed of trailing newlines -- used only to
# independently verify results the driver already produced, never to
# perform the operation under test. Only ever called with plain ASCII
# paths (no spaces/quotes -- see call sites below), single-quoted here
# for the device's shell rather than via printf %q, whose bash-specific
# quoting forms (e.g. $'...') Android's toybox sh does not understand.
remote_stat_mtime() {
    "$ADB_BIN" -s "$SERIAL" shell "stat -c %Y '$1'" | tr -d '\r\n'
}

remote_path_exists() {
    "$ADB_BIN" -s "$SERIAL" shell "test -e '$1' && echo YES || echo NO" | tr -d '\r\n'
}

echo "=== Step 1: device listing ==="
LIST_ROOT_OUT="$("$DRIVER" list /)"
echo "$LIST_ROOT_OUT"
case "$LIST_ROOT_OUT" in
    *"($SERIAL)"*|"$SERIAL"*)
        pass "device listing includes $SERIAL"
        ;;
    *)
        fail "device listing does not mention $SERIAL: $LIST_ROOT_OUT"
        ;;
esac
if [ -n "$MODEL" ]; then
    case "$LIST_ROOT_OUT" in
        *"$MODEL"*) pass "device listing shows model $MODEL" ;;
        *) fail "device listing does not show model $MODEL: $LIST_ROOT_OUT" ;;
    esac
fi

echo "=== Step 2: mkdir $TEST_ROOT ==="
assert_under_test_root "$TEST_ROOT"
"$DRIVER" mkdir "$WFX_ROOT" >/dev/null
LIST_SDCARD_OUT="$("$DRIVER" list "/$SERIAL/sdcard")"
case "$LIST_SDCARD_OUT" in
    *"adb_wfx_test"*) pass "adb_wfx_test appears under /sdcard" ;;
    *) fail "adb_wfx_test missing from /sdcard listing: $LIST_SDCARD_OUT" ;;
esac

echo "=== Step 3: upload with a fixed mtime (the acceptance criterion) ==="
FIXED_EPOCH="$(date -j -u -f "%Y-%m-%d %H:%M:%S" "2001-02-03 04:05:06" "+%s")"
ONEMIB_LOCAL="$LOCAL_SCRATCH/onemib.bin"
dd if=/dev/urandom of="$ONEMIB_LOCAL" bs=1048576 count=1 >/dev/null 2>&1
python3 - "$ONEMIB_LOCAL" "$FIXED_EPOCH" <<'PY'
import os, sys
path, epoch = sys.argv[1], int(sys.argv[2])
os.utime(path, (epoch, epoch))
PY
ONEMIB_REMOTE_WFX="$WFX_ROOT/onemib.bin"
ONEMIB_REMOTE_PATH="$TEST_ROOT/onemib.bin"
assert_under_test_root "$ONEMIB_REMOTE_PATH"
"$DRIVER" put "$ONEMIB_LOCAL" "$ONEMIB_REMOTE_WFX" >/dev/null
REMOTE_EPOCH="$(remote_stat_mtime "$ONEMIB_REMOTE_PATH")"
if [ "$REMOTE_EPOCH" = "$FIXED_EPOCH" ]; then
    pass "remote mtime ($REMOTE_EPOCH) matches the uploaded local mtime ($FIXED_EPOCH)"
else
    fail "remote mtime ($REMOTE_EPOCH) != expected ($FIXED_EPOCH)"
fi

echo "=== Step 4: download it back, compare mtime and bytes ==="
ONEMIB_DOWNLOADED="$LOCAL_SCRATCH/onemib.downloaded.bin"
"$DRIVER" get "$ONEMIB_REMOTE_WFX" "$ONEMIB_DOWNLOADED" >/dev/null
DOWNLOADED_EPOCH="$(stat -f "%m" "$ONEMIB_DOWNLOADED")"
if [ "$DOWNLOADED_EPOCH" = "$REMOTE_EPOCH" ]; then
    pass "downloaded local mtime ($DOWNLOADED_EPOCH) matches remote mtime"
else
    fail "downloaded local mtime ($DOWNLOADED_EPOCH) != remote mtime ($REMOTE_EPOCH)"
fi
SUM_UPLOADED="$(shasum -a 256 "$ONEMIB_LOCAL" | awk '{print $1}')"
SUM_DOWNLOADED="$(shasum -a 256 "$ONEMIB_DOWNLOADED" | awk '{print $1}')"
if [ "$SUM_UPLOADED" = "$SUM_DOWNLOADED" ]; then
    pass "downloaded bytes match uploaded bytes ($SUM_UPLOADED)"
else
    fail "checksum mismatch: uploaded=$SUM_UPLOADED downloaded=$SUM_DOWNLOADED"
fi

echo "=== Step 5: spaces, apostrophe, Cyrillic, and shell-metacharacter names ==="
SMALL_LOCAL="$LOCAL_SCRATCH/small.bin"
dd if=/dev/urandom of="$SMALL_LOCAL" bs=1024 count=4 >/dev/null 2>&1

NAME1="Вася's фото 1.jpg"
NAME2="Вася's фото 2.jpg"
REMOTE1_PATH="$TEST_ROOT/$NAME1"
REMOTE2_PATH="$TEST_ROOT/$NAME2"
WFX1="$WFX_ROOT/$NAME1"
WFX2="$WFX_ROOT/$NAME2"
assert_under_test_root "$REMOTE1_PATH"
assert_under_test_root "$REMOTE2_PATH"

"$DRIVER" put "$SMALL_LOCAL" "$WFX1" >/dev/null
LIST1="$("$DRIVER" list "$WFX_ROOT")"
case "$LIST1" in
    *"$NAME1"*) pass "created '$NAME1' is visible in the listing" ;;
    *) fail "'$NAME1' missing from listing after put" ;;
esac

"$DRIVER" mv "$WFX1" "$WFX2" >/dev/null
LIST2="$("$DRIVER" list "$WFX_ROOT")"
case "$LIST2" in
    *"$NAME2"*)
        case "$LIST2" in
            *"$NAME1"*) fail "'$NAME1' still present after rename to '$NAME2'" ;;
            *) pass "renamed '$NAME1' -> '$NAME2'" ;;
        esac
        ;;
    *) fail "'$NAME2' missing from listing after rename" ;;
esac

assert_under_test_root "$REMOTE2_PATH"
"$DRIVER" rm "$WFX2" >/dev/null
LIST3="$("$DRIVER" list "$WFX_ROOT")"
case "$LIST3" in
    *"$NAME2"*) fail "'$NAME2' still present after rm" ;;
    *) pass "deleted '$NAME2'" ;;
esac

# A filename containing a literal $(...) and `...` command-substitution
# form. Single-quoted so THIS script's own shell never expands it -- the
# only thing that matters is that it survives, verbatim, all the way
# through argv -> PluginCore -> shellQuote() -> the device's shell and
# back, proving the quoting holds end to end.
NAME3='inject-$(id)-`id`.txt'
REMOTE3_PATH="$TEST_ROOT/$NAME3"
WFX3="$WFX_ROOT/$NAME3"
assert_under_test_root "$REMOTE3_PATH"

"$DRIVER" put "$SMALL_LOCAL" "$WFX3" >/dev/null
LIST4="$("$DRIVER" list "$WFX_ROOT")"
case "$LIST4" in
    *"$NAME3"*) pass "created literal '\$(id)'/\`id\` filename is visible in the listing" ;;
    *) fail "'$NAME3' missing from listing after put" ;;
esac

assert_under_test_root "$REMOTE3_PATH"
"$DRIVER" rm "$WFX3" >/dev/null
LIST5="$("$DRIVER" list "$WFX_ROOT")"
case "$LIST5" in
    *"$NAME3"*) fail "'$NAME3' still present after rm" ;;
    *) pass "deleted literal '\$(id)'/\`id\` filename" ;;
esac

echo "=== Step 6: settime on a remote file ==="
SETTIME_EPOCH="$(date -j -u -f "%Y-%m-%d %H:%M:%S" "2010-05-06 07:08:09" "+%s")"
"$DRIVER" settime "$ONEMIB_REMOTE_WFX" "$SETTIME_EPOCH" >/dev/null
SETTIME_REMOTE_EPOCH="$(remote_stat_mtime "$ONEMIB_REMOTE_PATH")"
if [ "$SETTIME_REMOTE_EPOCH" = "$SETTIME_EPOCH" ]; then
    pass "settime set remote mtime to $SETTIME_EPOCH"
else
    fail "settime: remote mtime ($SETTIME_REMOTE_EPOCH) != expected ($SETTIME_EPOCH)"
fi

echo "=== Step 7: cancellation of a large transfer ==="
BIG_LOCAL="$LOCAL_SCRATCH/big.bin"
BIG_SIZE_MB=200
CANCEL_AFTER_BYTES=$((50 * 1024 * 1024)) # cancel partway through, well before EOF
dd if=/dev/zero of="$BIG_LOCAL" bs=1048576 count="$BIG_SIZE_MB" >/dev/null 2>&1
BIG_REMOTE_WFX="$WFX_ROOT/big.bin"
BIG_REMOTE_PATH="$TEST_ROOT/big.bin"

START_TIME=$(date +%s)
set +e
"$DRIVER" put "$BIG_LOCAL" "$BIG_REMOTE_WFX" "$CANCEL_AFTER_BYTES" >/tmp/device_test_cancel_out.$$ 2>&1
PUT_EXIT=$?
set -e
END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))
CANCEL_OUTPUT="$(cat /tmp/device_test_cancel_out.$$)"
rm -f /tmp/device_test_cancel_out.$$

echo "cancel put exit=$PUT_EXIT elapsed=${ELAPSED}s output=$CANCEL_OUTPUT"
if [ "$PUT_EXIT" -eq 2 ] && [ "$CANCEL_OUTPUT" = "ABORTED" ]; then
    pass "driver reported ABORTED (exit 2) for the cancelled transfer"
else
    fail "expected exit 2 / 'ABORTED' for the cancelled transfer, got exit=$PUT_EXIT output='$CANCEL_OUTPUT'"
fi
if [ "$ELAPSED" -le 60 ]; then
    pass "cancellation was prompt (${ELAPSED}s)"
else
    fail "cancellation took too long (${ELAPSED}s) -- did not exit promptly"
fi

# Best-effort: the partial file is inside TEST_ROOT and step 8's rm -rf
# below removes it anyway; no separate cleanup needed here.
assert_under_test_root "$BIG_REMOTE_PATH"

echo "=== Step 8: cleanup ==="
assert_under_test_root "$TEST_ROOT"
"$DRIVER" rmdir "$WFX_ROOT" >/dev/null
EXISTS_AFTER="$(remote_path_exists "$TEST_ROOT")"
if [ "$EXISTS_AFTER" = "NO" ]; then
    pass "$TEST_ROOT is gone after cleanup"
else
    fail "$TEST_ROOT still exists after rmdir"
fi

echo
echo "ALL DEVICE CHECKS PASSED"
