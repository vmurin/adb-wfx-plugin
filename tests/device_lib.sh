# tests/device_lib.sh -- shared setup for the two scripts that touch a
# real Android device (tests/device_test.sh, tests/bench.sh). Not
# executable on its own; sourced with `. tests/device_lib.sh`.
#
# Centralises the one safety-critical constant both scripts must never
# drift from: TEST_ROOT, the *only* on-device directory this whole test
# suite is allowed to touch, plus assert_under_test_root(), the guard
# every destructive on-device command must pass through before it runs.

# The single named constant for the sandbox this entire test suite is
# confined to on the device. Every path device_test.sh or bench.sh ever
# creates, writes into, or removes on the phone MUST be this path or a
# path under it -- see assert_under_test_root() below.
readonly TEST_ROOT="/sdcard/adb_wfx_test"

# Exit code both device_test.sh and bench.sh use to mean "no device is
# attached" -- distinct from 1 (a genuine assertion/test failure) and 3
# (device_driver's own usage error) so a human or a CI runner can tell
# "there was nothing to test" apart from "the test ran and failed".
readonly EXIT_NO_DEVICE=42

# Refuses (exit 1) unless $1 is exactly TEST_ROOT or a path under it.
# Call this immediately before every destructive on-device command (rm,
# rmdir, and any mv whose *destination* could plausibly land outside the
# sandbox) -- belt and braces against a path-construction bug turning a
# test run into damage on a real person's device. Never call this on an
# empty or unset string: an empty argument fails the case match below and
# is refused, exactly like a wrong path would be.
assert_under_test_root() {
    case "$1" in
        "$TEST_ROOT"|"$TEST_ROOT"/*)
            return 0
            ;;
        *)
            echo "REFUSING to operate outside $TEST_ROOT: '$1'" >&2
            exit 1
            ;;
    esac
}

# The WFX-path counterpart of assert_under_test_root(): checks the EXACT
# string a device_driver rm/rmdir/mv call is about to receive (a
# "/<serial>/<on-device-path>" WFX path), not a look-alike on-device-only
# path -- the two are easy to conflate, and a guard that checks the wrong
# variable is worse than no guard at all (it passes review while
# asserting nothing about what's actually destroyed). Requires $SERIAL to
# already be set (require_device does this). Call this immediately
# before every destructive device_driver invocation, on the literal
# argument being passed to it.
assert_wfx_under_test_root() {
    local wfx_test_root="/$SERIAL$TEST_ROOT"
    case "$1" in
        "$wfx_test_root"|"$wfx_test_root"/*)
            return 0
            ;;
        *)
            echo "REFUSING a device_driver call outside $wfx_test_root: '$1'" >&2
            exit 1
            ;;
    esac
}

# Finds an adb binary (respecting $ADB_PATH first, like adbserver.hpp
# does), starts its server, and picks the first device in the "device"
# state from `adb devices -l`. On any failure to find adb or a usable
# device, prints a clearly-labelled message and exits $EXIT_NO_DEVICE --
# never silently continues, never fakes a result.
#
# Sets: ADB_BIN, SERIAL, MODEL (MODEL may be empty).
require_device() {
    ADB_BIN="${ADB_PATH:-}"
    if [ -z "$ADB_BIN" ]; then
        ADB_BIN="$(command -v adb || true)"
    fi
    if [ -z "$ADB_BIN" ]; then
        echo "$(basename "$0"): NO DEVICE -- adb binary not found." >&2
        echo "Connect a phone with USB debugging enabled, install platform-tools, and re-run." >&2
        exit "$EXIT_NO_DEVICE"
    fi

    "$ADB_BIN" start-server >/dev/null 2>&1 || true

    DEVICE_LINE="$("$ADB_BIN" devices -l | awk '$2=="device"{print; exit}')"
    if [ -z "$DEVICE_LINE" ]; then
        echo "$(basename "$0"): NO DEVICE -- no device attached." >&2
        echo "Connect a phone with USB debugging enabled and re-run." >&2
        exit "$EXIT_NO_DEVICE"
    fi

    SERIAL="$(echo "$DEVICE_LINE" | awk '{print $1}')"
    MODEL="$(echo "$DEVICE_LINE" | grep -o 'model:[^ ]*' | cut -d: -f2 || true)"
    if [ -z "$SERIAL" ]; then
        echo "$(basename "$0"): NO DEVICE -- could not parse a serial from 'adb devices -l'." >&2
        exit "$EXIT_NO_DEVICE"
    fi
    echo "$(basename "$0"): using device $SERIAL (model ${MODEL:-unknown})"
}

# Builds tests/bin/device_driver fresh, the same way run_tests.sh does
# under ADB_WFX_DEVICE_TESTS=1, so a script run standalone (without having
# just run run_tests.sh) still has a driver to invoke.
build_driver() {
    mkdir -p tests/bin
    clang++ -std=c++17 -Wall -Wextra -Werror -I. tests/device_driver.cpp -o tests/bin/device_driver
    DRIVER="$(pwd)/tests/bin/device_driver"
}
