# Contributing

Thanks for taking the time. Bug reports are welcome, and **reports from Linux
users are especially useful** — those builds pass CI but have never been run
against a real device, so any first-hand account moves the project forward.

## Building and testing

```sh
./build.sh          # -> fsplugin.wfx64
./run_tests.sh      # the full unit suite; must end with ALL CHECKS PASSED
```

`run_tests.sh` needs no Android device: everything runs against
`tests/fake_transport.hpp`. It builds the plugin, runs every `tests/test_*.cpp`,
asserts the exported symbol table, and compiles `tests/device_driver.cpp`.

Two suites do need hardware and are opt-in:

```sh
ADB_WFX_DEVICE_TESTS=1 ./tests/device_test.sh   # end-to-end against a phone
./tests/bench.sh                                # plugin vs. adb push/pull
```

Everything they touch on the device lives under `/sdcard/adb_wfx_test/`, and they
clean up after themselves. With no device attached they exit 42 rather than
failing.

## Ground rules for changes

- **`-Werror` stays on.** Every build in this project is warning-free on both
  clang and gcc, and that is not negotiable for a library loaded into someone
  else's file manager.
- **New behaviour comes with tests.** The unit suite covers the wire protocol
  byte for byte in places; follow the surrounding style rather than testing
  through the UI.
- **Keep `FsSetTimeW` exported.** Double Commander silently stops copying file
  dates — in both directions — if it is missing. `scripts/check-exports.sh`
  fails loudly on this for a reason.
- **The SDK headers are vendored and never edited.** `wfxplugin.h` and
  `common.h` come from `doublecmd/doublecmd`'s `sdk/` directory as-is, and only
  `sdk.h` may include them.
- **One seam per concern.** `transport.hpp` is the only file that touches a
  socket, `adbproto.hpp` is a pure codec with no I/O, and `fsplugin.cpp` is the
  only translation unit. Please keep it that way.

## Commit messages

Write a short subject line in the imperative, then a body that explains *why*
the change is right — what the old behaviour was, what went wrong with it, and
what was considered and rejected. The existing history is the style guide; it is
deliberately more explanatory than most.

## Pull requests

- Branch off `main`.
- Make sure `./run_tests.sh` is green before opening the PR, and say in the
  description which platform you ran it on.
- If you have a device and the change touches transfer behaviour, run
  `tests/device_test.sh` too and say so.
- CI runs the unit suite on macOS (arm64, x86_64) and Linux (x86_64, aarch64).

## Releasing (maintainers)

1. Update `version.h` and add the section to `CHANGELOG.md` — CI checks that
   they agree.
2. `git tag -a vX.Y.Z -m "..."` and push the tag.
3. The release workflow builds all three archives, checksums them, and publishes
   the release. A tag containing a hyphen (`v1.0.0-rc1`) is published as a
   prerelease.
