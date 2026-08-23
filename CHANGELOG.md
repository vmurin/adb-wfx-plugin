# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.0.0] - 2026-08-24

First public release.

### Added

- WFX file-system plugin for Double Commander that browses Android devices over
  ADB, speaking the `adb` server's wire protocol directly (`host:`, `sync` v1 and
  `shell:` services) instead of shelling out to the `adb` command line for every
  operation.
- Devices appear under a virtual `ADB` root as `<model> (<serial>)`.
- Upload and download with the **real modification time preserved in both
  directions** — the reason this plugin exists, since MTP stamps uploads with the
  moment of the copy and offers no way to correct it.
- Delete, rename/move, copy within one device, and create directory.
- `FsSetTimeW`, wired to Double Commander's own "change date" UI. It is also
  load-bearing for a DC quirk: DC strips its internal `caoCopyTime` flag from
  *every* copy — downloads included — unless a plugin exports `FsSetTime` or
  `FsSetTimeW` ([doublecmd#3051](https://github.com/doublecmd/doublecmd/issues/3051)).
- Correct handling of filenames with spaces, quotes, non-ASCII characters and
  literal shell metacharacters: every path sent to the device's shell is
  single-quoted first.
- Directory-listing cache with a short TTL and a mutex, so stepping back into a
  directory you just left costs no round trip.
- Cancellable transfers, socket timeouts with a stall hook, and bounded
  wire-driven allocations.
- Unit suite of 880+ checks running against a fake transport (no device needed),
  an export-table check, an opt-in end-to-end suite against real hardware
  (`tests/device_test.sh`) and a benchmark against `adb push`/`adb pull`
  (`tests/bench.sh`).
- Builds for macOS (arm64, x86_64, universal) and Linux (x86_64, aarch64).

### Known limitations

See the README for the full list. In brief: sync protocol v1 only (32-bit mtime,
so no dates past 2038; 32-bit size, so no files ≥ 4 GiB), single-threaded use
only, permission-denied directories are indistinguishable from empty ones, and
F6 to or from a local disk is refused by Double Commander itself before the
plugin is asked to do anything.

[Unreleased]: https://github.com/vmurin/adb-wfx-plugin/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/vmurin/adb-wfx-plugin/releases/tag/v1.0.0
