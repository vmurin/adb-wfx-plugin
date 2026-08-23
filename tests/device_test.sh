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
assert_wfx_under_test_root "$WFX2"
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
assert_wfx_under_test_root "$WFX3"
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

echo "=== Step 6b: the touch -t fallback (TZ correctness on a non-UTC phone) ==="
# settime above may well have succeeded on the "-d @<epoch>" form, in
# which case the "-t" fallback -- the one that has to be sent under
# TZ=UTC, because touch -t reads its argument in the DEVICE's local time
# -- was never exercised at all. settime-t forces it. If the TZ=UTC prefix
# were missing, this step fails by exactly the phone's UTC offset, which
# is the whole point of running it on a phone that is not set to UTC.
FALLBACK_EPOCH="$(date -j -u -f "%Y-%m-%d %H:%M:%S" "2013-11-12 13:14:15" "+%s")"
"$DRIVER" settime-t "$ONEMIB_REMOTE_WFX" "$FALLBACK_EPOCH" >/dev/null
FALLBACK_REMOTE_EPOCH="$(remote_stat_mtime "$ONEMIB_REMOTE_PATH")"
if [ "$FALLBACK_REMOTE_EPOCH" = "$FALLBACK_EPOCH" ]; then
    pass "touch -t fallback set remote mtime to $FALLBACK_EPOCH"
else
    OFFSET=$((FALLBACK_REMOTE_EPOCH - FALLBACK_EPOCH))
    fail "touch -t fallback: remote mtime ($FALLBACK_REMOTE_EPOCH) != expected ($FALLBACK_EPOCH), off by ${OFFSET}s -- is the TZ=UTC prefix missing?"
fi

echo "=== Step 6c: a nonexistent directory is an error, not an empty listing ==="
# adbd answers a failed opendir() with an empty DONE, never a FAIL, so
# without an explicit STAT this reads as a successful empty directory and
# Double Commander shows a blank panel with no message.
set +e
MISSING_OUT="$("$DRIVER" list "$WFX_ROOT/definitely_not_here" 2>&1)"
MISSING_EXIT=$?
set -e
if [ "$MISSING_EXIT" -ne 0 ]; then
    case "$MISSING_OUT" in
        *"no such file or directory"*)
            pass "listing a nonexistent directory failed with a clear message"
            ;;
        *)
            fail "listing a nonexistent directory failed, but without a clear message: $MISSING_OUT"
            ;;
    esac
else
    fail "listing a nonexistent directory succeeded (as an empty listing): $MISSING_OUT"
fi

echo "=== Step 6d: /sdcard is a symlink and must still open as a directory ==="
# lstat() reports /sdcard as mode 0120777 on every modern device; if the
# plugin classifies it by that alone, the most common destination on the
# phone renders as a 21-byte file and cannot be opened at all.
SDCARD_LIST="$("$DRIVER" list "/$SERIAL/sdcard")"
if [ -n "$SDCARD_LIST" ]; then
    pass "/sdcard opens as a directory and lists $(echo "$SDCARD_LIST" | wc -l | tr -d ' ') entries"
else
    fail "/sdcard listed as empty -- is the symlink being treated as a file?"
fi
# ... and the device root must show it AS a directory (trailing field 1).
ROOT_LIST="$("$DRIVER" list "/$SERIAL")"
SDCARD_ROW="$(echo "$ROOT_LIST" | awk -F'\t' '$1=="sdcard"{print; exit}')"
if [ -z "$SDCARD_ROW" ]; then
    fail "no 'sdcard' entry in the device root listing"
fi
case "$SDCARD_ROW" in
    *$'\t'1) pass "device root lists 'sdcard' as a directory" ;;
    *) fail "device root lists 'sdcard' as a file: $SDCARD_ROW" ;;
esac

echo "=== Step 6e: the listing cache holds briefly, then expires ==="
# One driver process, one PluginCore, one cache. A file is pushed into
# $TEST_ROOT from outside while the driver sleeps between its first and
# second listing: the second must still show the OLD count (the cache is
# genuinely serving) and the third the NEW one (the TTL genuinely
# elapsed). Before the TTL existed, the third count matched the first and
# a photo taken on the phone never appeared until DC was restarted.
CACHE_PROBE_LOCAL="$LOCAL_SCRATCH/cache_probe.bin"
dd if=/dev/urandom of="$CACHE_PROBE_LOCAL" bs=1024 count=1 >/dev/null 2>&1
CACHE_PROBE_PATH="$TEST_ROOT/cache_probe.bin"
assert_under_test_root "$CACHE_PROBE_PATH"
# Timing, against LISTING_CACHE_TTL_SECONDS = 5: the driver lists at
# t=0, t=4 (still fresh) and t=8 (expired), and the push lands at t~2.
# It therefore assumes the driver reaches its first listing within two
# seconds of being started, which it does against an adb server
# require_device has already warmed up.
(
    sleep 2
    "$ADB_BIN" -s "$SERIAL" push "$CACHE_PROBE_LOCAL" "$CACHE_PROBE_PATH" >/dev/null 2>&1
) &
CACHE_PUSH_PID=$!
CACHE_OUT="$("$DRIVER" cachettl "$WFX_ROOT" 4 4)"
wait "$CACHE_PUSH_PID" || true
echo "$CACHE_OUT"
CACHE_FIRST="$(echo "$CACHE_OUT" | awk '$1=="FIRST"{print $2}')"
CACHE_CACHED="$(echo "$CACHE_OUT" | awk '$1=="CACHED"{print $2}')"
CACHE_EXPIRED="$(echo "$CACHE_OUT" | awk '$1=="EXPIRED"{print $2}')"
if [ -z "$CACHE_FIRST" ] || [ -z "$CACHE_CACHED" ] || [ -z "$CACHE_EXPIRED" ]; then
    fail "cachettl did not print all three counts: $CACHE_OUT"
fi
if [ "$CACHE_CACHED" = "$CACHE_FIRST" ]; then
    pass "the cache served the earlier listing ($CACHE_CACHED entries) while it was still fresh"
else
    fail "cached listing changed before the TTL: first=$CACHE_FIRST cached=$CACHE_CACHED"
fi
if [ "$CACHE_EXPIRED" -gt "$CACHE_FIRST" ]; then
    pass "the cache expired and picked up the externally-added file ($CACHE_EXPIRED entries)"
else
    fail "cache never expired: first=$CACHE_FIRST expired=$CACHE_EXPIRED -- an externally-added file stayed invisible"
fi

echo "=== Step 7: cancellation of a large transfer ==="
BIG_LOCAL="$LOCAL_SCRATCH/big.bin"
BIG_SIZE_MB=200
CANCEL_AFTER_BYTES=$((50 * 1024 * 1024)) # cancel partway through, well before EOF
dd if=/dev/zero of="$BIG_LOCAL" bs=1048576 count="$BIG_SIZE_MB" >/dev/null 2>&1
BIG_REMOTE_WFX="$WFX_ROOT/big.bin"
BIG_REMOTE_PATH="$TEST_ROOT/big.bin"

# stdout and stderr are captured SEPARATELY (both inside LOCAL_SCRATCH,
# so the trap cleans them up even if fail() below exits early): a cold
# adb server prints its own "* daemon not running..." startup message
# (from startAdbServer's spawned "adb start-server"), which lands on
# whichever fd adb chooses to use and must never be allowed to corrupt
# the driver's own single-word stdout output. The match is a substring
# check (not exact equality) for the same reason: only "ABORTED"
# appearing on stdout matters, not stdout being *nothing but* that word.
CANCEL_STDOUT="$LOCAL_SCRATCH/cancel.stdout"
CANCEL_STDERR="$LOCAL_SCRATCH/cancel.stderr"
START_TIME=$(date +%s)
set +e
"$DRIVER" put "$BIG_LOCAL" "$BIG_REMOTE_WFX" "$CANCEL_AFTER_BYTES" >"$CANCEL_STDOUT" 2>"$CANCEL_STDERR"
PUT_EXIT=$?
set -e
END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))
CANCEL_OUTPUT="$(cat "$CANCEL_STDOUT")"

echo "cancel put exit=$PUT_EXIT elapsed=${ELAPSED}s stdout=$CANCEL_OUTPUT stderr=$(cat "$CANCEL_STDERR")"
case "$CANCEL_OUTPUT" in
    *ABORTED*)
        if [ "$PUT_EXIT" -eq 2 ]; then
            pass "driver reported ABORTED (exit 2) for the cancelled transfer"
        else
            fail "driver printed ABORTED but exited $PUT_EXIT (expected 2)"
        fi
        ;;
    *)
        fail "expected exit 2 / 'ABORTED' for the cancelled transfer, got exit=$PUT_EXIT stdout='$CANCEL_OUTPUT'"
        ;;
esac
if [ "$ELAPSED" -le 60 ]; then
    pass "cancellation was prompt (${ELAPSED}s)"
else
    fail "cancellation took too long (${ELAPSED}s) -- did not exit promptly"
fi

# Best-effort: the partial file is inside TEST_ROOT and step 8's rm -rf
# below removes it anyway; no separate cleanup needed here.
assert_under_test_root "$BIG_REMOTE_PATH"

echo "=== Step 7b: cancellation of a large DOWNLOAD ==="
# Step 7 only covers the upload direction. Downloads cancel on a
# different path (syncRecv, plus the temp-file-and-rename dance in
# getFile), and the thing that must not happen is a truncated file left
# sitting at the real target name looking like a complete copy.
DL_LOCAL="$LOCAL_SCRATCH/dlsource.bin"
DL_SIZE_MB=60
DL_CANCEL_AFTER_BYTES=$((15 * 1024 * 1024))
dd if=/dev/zero of="$DL_LOCAL" bs=1048576 count="$DL_SIZE_MB" >/dev/null 2>&1
DL_REMOTE_WFX="$WFX_ROOT/dlsource.bin"
DL_REMOTE_PATH="$TEST_ROOT/dlsource.bin"
assert_under_test_root "$DL_REMOTE_PATH"
"$DRIVER" put "$DL_LOCAL" "$DL_REMOTE_WFX" >/dev/null

DL_TARGET="$LOCAL_SCRATCH/dl.cancelled.bin"
DL_STDOUT="$LOCAL_SCRATCH/dlcancel.stdout"
DL_STDERR="$LOCAL_SCRATCH/dlcancel.stderr"
DL_START=$(date +%s)
set +e
"$DRIVER" get "$DL_REMOTE_WFX" "$DL_TARGET" "$DL_CANCEL_AFTER_BYTES" >"$DL_STDOUT" 2>"$DL_STDERR"
GET_EXIT=$?
set -e
DL_ELAPSED=$(($(date +%s) - DL_START))
DL_OUTPUT="$(cat "$DL_STDOUT")"

echo "cancel get exit=$GET_EXIT elapsed=${DL_ELAPSED}s stdout=$DL_OUTPUT stderr=$(cat "$DL_STDERR")"
case "$DL_OUTPUT" in
    *ABORTED*)
        if [ "$GET_EXIT" -eq 2 ]; then
            pass "driver reported ABORTED (exit 2) for the cancelled download"
        else
            fail "driver printed ABORTED but exited $GET_EXIT (expected 2)"
        fi
        ;;
    *)
        fail "expected exit 2 / 'ABORTED' for the cancelled download, got exit=$GET_EXIT stdout='$DL_OUTPUT'"
        ;;
esac
if [ -e "$DL_TARGET" ]; then
    fail "a cancelled download left a file at $DL_TARGET -- a partial copy must never appear under the real name"
else
    pass "no file was left behind at the download target"
fi
# ... and no temp file either.
LEFTOVER_TEMPS="$(find "$LOCAL_SCRATCH" -name 'dl.cancelled.bin.adbwfx.tmp.*' | wc -l | tr -d ' ')"
if [ "$LEFTOVER_TEMPS" = "0" ]; then
    pass "no temporary download file was left behind"
else
    fail "$LEFTOVER_TEMPS temporary download file(s) left behind in $LOCAL_SCRATCH"
fi
if [ "$DL_ELAPSED" -le 60 ]; then
    pass "download cancellation was prompt (${DL_ELAPSED}s)"
else
    fail "download cancellation took too long (${DL_ELAPSED}s)"
fi

echo "=== Step 8: cleanup ==="
assert_under_test_root "$TEST_ROOT"
assert_wfx_under_test_root "$WFX_ROOT"
"$DRIVER" rmdir "$WFX_ROOT" >/dev/null
EXISTS_AFTER="$(remote_path_exists "$TEST_ROOT")"
if [ "$EXISTS_AFTER" = "NO" ]; then
    pass "$TEST_ROOT is gone after cleanup"
else
    fail "$TEST_ROOT still exists after rmdir"
fi

echo
echo "ALL DEVICE CHECKS PASSED"
