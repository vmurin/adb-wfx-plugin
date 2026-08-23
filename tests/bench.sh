#!/usr/bin/env bash
# tests/bench.sh -- compares raw `adb push`/`adb pull` of a directory
# (the baseline) against this plugin's own put/get, driven through
# tests/device_driver.cpp exactly the way tests/device_test.sh does, over
# the same 100-file corpus. Prints a table of seconds and MB/s (median of
# 5 runs each) plus the plugin/adb ratio.
#
# The spec's target is plugin throughput close to raw adb (~32 MB/s
# pull), not MTP-level (~19 MB/s) -- this script only measures and
# reports; it never fails the build on the numbers.
#
# Both arms must have the SAME shape or the numbers mean nothing. `adb
# push`/`adb pull` move the whole corpus in one process; the plugin arm
# therefore uses device_driver's putmany/getmany, which do the same over
# one PluginCore. An earlier version of this script ran `device_driver
# put` once per file -- 100 process launches per run, each of which also
# spawned `adb start-server` -- and would have reported MTP-level numbers
# for a plugin that is actually fast.
#
# Safety: builds its local corpus in a FRESH temp directory and removes
# only that directory (never an ambient one); every on-device path is
# $TEST_ROOT or a path under it, guarded by assert_under_test_root()
# (tests/device_lib.sh) before any destructive command.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tests/device_lib.sh
. tests/device_lib.sh

if ! command -v bc >/dev/null 2>&1; then
    echo "bench.sh: 'bc' is required (used for the MB/s and ratio arithmetic) but was not found." >&2
    echo "Install it (e.g. via Homebrew: 'brew install bc') and re-run." >&2
    exit 1
fi

require_device
build_driver

RUNS=5
CORPUS_FILE_COUNT=100
MIN_FILE_BYTES=1500000   # 1.5 MB (decimal)
MAX_FILE_BYTES=3000000   # 3.0 MB (decimal)
MEGABYTE=1000000         # decimal MB, matching the spec's measured numbers

REMOTE_BENCH_DIR="$TEST_ROOT/bench"
REMOTE_ADB_DIR="$REMOTE_BENCH_DIR/adb"
REMOTE_PLUGIN_DIR="$REMOTE_BENCH_DIR/plugin"
WFX_PLUGIN_DIR="/$SERIAL$REMOTE_PLUGIN_DIR"

assert_under_test_root "$REMOTE_BENCH_DIR"
assert_under_test_root "$REMOTE_ADB_DIR"
assert_under_test_root "$REMOTE_PLUGIN_DIR"

# --- local scratch space: this is the ONLY directory this script removes
# on the local filesystem, and only this one. ---
LOCAL_TMP_ROOT="$(mktemp -d)"
CORPUS_DIR="$LOCAL_TMP_ROOT/corpus"
DOWNLOAD_ADB_DIR="$LOCAL_TMP_ROOT/download-adb"
DOWNLOAD_PLUGIN_DIR="$LOCAL_TMP_ROOT/download-plugin"
mkdir -p "$CORPUS_DIR" "$DOWNLOAD_ADB_DIR" "$DOWNLOAD_PLUGIN_DIR"

cleanup_local() {
    rm -rf "$LOCAL_TMP_ROOT"
}
trap cleanup_local EXIT

echo "Building corpus of $CORPUS_FILE_COUNT files in $CORPUS_DIR ..."
TOTAL_BYTES=0
for i in $(seq 1 "$CORPUS_FILE_COUNT"); do
    span=$((MAX_FILE_BYTES - MIN_FILE_BYTES + 1))
    size=$(( (RANDOM * 32768 + RANDOM) % span + MIN_FILE_BYTES ))
    file="$CORPUS_DIR/file_$(printf '%03d' "$i").bin"
    head -c "$size" /dev/urandom > "$file"
    TOTAL_BYTES=$((TOTAL_BYTES + size))
done
TOTAL_MB=$(echo "scale=1; $TOTAL_BYTES / $MEGABYTE" | bc)
echo "Corpus built: $CORPUS_FILE_COUNT files, ${TOTAL_BYTES} bytes (~${TOTAL_MB} MB)."

# Recreate the on-device bench directories from a known-clean state.
assert_under_test_root "$REMOTE_BENCH_DIR"
"$ADB_BIN" -s "$SERIAL" shell "rm -rf '$REMOTE_BENCH_DIR'" >/dev/null 2>&1 || true
"$DRIVER" mkdir "/$SERIAL$REMOTE_BENCH_DIR" >/dev/null
"$DRIVER" mkdir "/$SERIAL$REMOTE_ADB_DIR" >/dev/null
"$DRIVER" mkdir "$WFX_PLUGIN_DIR" >/dev/null

TIMEFORMAT='%R'

median() {
    # Prints the median of the numeric arguments (RUNS is always odd).
    printf '%s\n' "$@" | sort -n | awk -v n="$#" '{a[NR]=$1} END{print a[int((n+1)/2)]}'
}

mb_per_sec() {
    # $1 = seconds, computed with bc for decimal division.
    echo "scale=2; $TOTAL_BYTES / $MEGABYTE / $1" | bc
}

echo
echo "=== adb push (baseline) ==="
adb_push_times=()
for run in $(seq 1 "$RUNS"); do
    assert_under_test_root "$REMOTE_ADB_DIR"
    "$ADB_BIN" -s "$SERIAL" shell "rm -rf '$REMOTE_ADB_DIR'" >/dev/null 2>&1 || true
    "$ADB_BIN" -s "$SERIAL" shell "mkdir -p '$REMOTE_ADB_DIR'" >/dev/null
    elapsed=$( { time "$ADB_BIN" -s "$SERIAL" push "$CORPUS_DIR"/. "$REMOTE_ADB_DIR" >/dev/null 2>&1; } 2>&1 | tail -1 )
    echo "  run $run: ${elapsed}s"
    adb_push_times+=("$elapsed")
done
adb_push_median="$(median "${adb_push_times[@]}")"

echo
echo "=== adb pull (baseline) ==="
adb_pull_times=()
for run in $(seq 1 "$RUNS"); do
    rm -rf "$DOWNLOAD_ADB_DIR"
    mkdir -p "$DOWNLOAD_ADB_DIR"
    elapsed=$( { time "$ADB_BIN" -s "$SERIAL" pull "$REMOTE_ADB_DIR" "$DOWNLOAD_ADB_DIR" >/dev/null 2>&1; } 2>&1 | tail -1 )
    echo "  run $run: ${elapsed}s"
    adb_pull_times+=("$elapsed")
done
adb_pull_median="$(median "${adb_pull_times[@]}")"

echo
echo "=== plugin driver put (tests/device_driver.cpp, through PluginCore) ==="
plugin_put_times=()
for run in $(seq 1 "$RUNS"); do
    assert_under_test_root "$REMOTE_PLUGIN_DIR"
    "$ADB_BIN" -s "$SERIAL" shell "rm -rf '$REMOTE_PLUGIN_DIR'" >/dev/null 2>&1 || true
    "$DRIVER" mkdir "$WFX_PLUGIN_DIR" >/dev/null
    elapsed=$( { time "$DRIVER" putmany "$CORPUS_DIR" "$WFX_PLUGIN_DIR" >/dev/null 2>&1; } 2>&1 | tail -1 )
    echo "  run $run: ${elapsed}s"
    plugin_put_times+=("$elapsed")
done
plugin_put_median="$(median "${plugin_put_times[@]}")"

echo
echo "=== plugin driver get (tests/device_driver.cpp, through PluginCore) ==="
plugin_get_times=()
for run in $(seq 1 "$RUNS"); do
    rm -rf "$DOWNLOAD_PLUGIN_DIR"
    mkdir -p "$DOWNLOAD_PLUGIN_DIR"
    elapsed=$( { time "$DRIVER" getmany "$WFX_PLUGIN_DIR" "$DOWNLOAD_PLUGIN_DIR" >/dev/null 2>&1; } 2>&1 | tail -1 )
    echo "  run $run: ${elapsed}s"
    plugin_get_times+=("$elapsed")
done
plugin_get_median="$(median "${plugin_get_times[@]}")"

# Best-effort on-device cleanup -- guarded, and confined to the bench
# subtree only.
assert_under_test_root "$REMOTE_BENCH_DIR"
"$ADB_BIN" -s "$SERIAL" shell "rm -rf '$REMOTE_BENCH_DIR'" >/dev/null 2>&1 || true

echo
echo "=================================================================="
printf "%-12s %10s %10s %10s\n" "operation" "median(s)" "MB/s" "ratio"
printf "%-12s %10s %10s %10s\n" "adb push" "$adb_push_median" "$(mb_per_sec "$adb_push_median")" "1.00"
printf "%-12s %10s %10s %10s\n" "plugin put" "$plugin_put_median" "$(mb_per_sec "$plugin_put_median")" \
    "$(echo "scale=2; $adb_push_median / $plugin_put_median" | bc)"
printf "%-12s %10s %10s %10s\n" "adb pull" "$adb_pull_median" "$(mb_per_sec "$adb_pull_median")" "1.00"
printf "%-12s %10s %10s %10s\n" "plugin get" "$plugin_get_median" "$(mb_per_sec "$plugin_get_median")" \
    "$(echo "scale=2; $adb_pull_median / $plugin_get_median" | bc)"
echo "=================================================================="
echo "Corpus: $CORPUS_FILE_COUNT files, ${TOTAL_BYTES} bytes (~${TOTAL_MB} MB), median of $RUNS runs each."
