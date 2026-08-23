# adb-wfx: an ADB file-system plugin for Double Commander

A Double Commander [WFX file-system plugin](https://doublecmd.sourceforge.io/)
that browses, copies, renames, and deletes files on an Android device over
ADB (Android Debug Bridge), talking to the `adb` server's wire protocol
directly rather than shelling out to the `adb` command-line tool for every
operation.

## Why this exists

Double Commander already ships an MTP plugin for Android devices. This one
exists for two reasons.

**Modification dates survive.** Over MTP, an uploaded file ends up stamped
with the moment of the copy rather than the date it actually had, and the
device offers no way to correct it. ADB's `sync` protocol carries the real
mtime in both directions, so a file keeps its date whether you copy it to
the phone or from it.

**It is faster.** ADB moves the same corpus roughly **2.6× faster than MTP
when writing to the device and 1.7× faster when reading from it**, and with
far less run-to-run variance. This plugin is built on ADB specifically to
inherit that.

## Building

Requires `clang++` with C++17 support.

```sh
./compile_mac.sh
```

produces `fsplugin.wfx64`, a Mach-O shared library exporting the 14 symbols
Double Commander's WFX interface requires (`FsInitW`, `FsFindFirstW`, ...,
including `FsSetTimeW` — see [the FsSetTimeW gotcha](#the-fssettimew-gotcha)
below).

For a universal arm64 + x86_64 binary, use `./compile_mac_universal.sh`
instead.

## Installing

1. Build the plugin, then run:

   ```sh
   ./scripts/install.sh
   ```

   It copies `fsplugin.wfx64` into Double Commander's plugin directory
   (`.../Double Commander.app/Contents/MacOS/plugins/wfx/adb/`), creating
   that subdirectory if needed and backing up any file already there to
   `fsplugin.wfx64.bak-<timestamp>` first.

2. Quit Double Commander completely and relaunch it.

3. If the ADB file system doesn't appear automatically, open
   **Configuration → Plugins → WFX plugins**, click **Add/Configure**, and
   point it at the installed `fsplugin.wfx64`.

4. Enable USB debugging on the device and authorise the computer when the
   phone prompts.

Devices then appear as directories under a virtual `ADB` root, named
`<model> (<serial>)` — for example `/<model> (<serial>)/sdcard/...`.

## What works

- Browsing the device's filesystem.
- Uploading and downloading files with the real modification time preserved
  in both directions.
- Delete, rename/move and copy (within one device), create directory.
- Setting a file's modification date from Double Commander's own "change
  date" UI (`FsSetTimeW`).
- Filenames containing spaces, quotes, and non-ASCII (e.g. Cyrillic)
  characters, and filenames that happen to look like shell metacharacters
  (`$(...)`, `` `...` ``) — every path sent to the device's shell is
  single-quoted before it gets there.

All of the above is covered by the unit suite and verified end to end
against a real Android device (see [Testing](#testing)).

## Known limitations

- **Sync protocol v1 only.** This plugin speaks the same wire protocol as
  the stock `adb` client, which has two hard limits baked into its packet
  format:
  - **32-bit mtime**: timestamps are a 32-bit count of seconds since the
    epoch, so dates past **January 19, 2038** cannot be represented (the
    "Y2038 problem").
  - **32-bit size**: file sizes are likewise 32-bit, so files **4 GiB or
    larger** cannot be transferred.
- **File transfer only.** This is a file-transfer plugin rather than a
  device-management tool.
- **Single-threaded use only.** The plugin keeps one client, one
  directory-listing cache, and one core instance for its entire lifetime.
  Double Commander's background/multi-threaded transfer modes call into a
  WFX plugin from more than one thread at once, and this plugin supports
  only its normal, single-threaded copy/move/browse operations. The listing
  cache itself is mutex-guarded, but that protects one container; it does
  not make the plugin as a whole thread-safe.
- **Directory listings can be up to a few seconds stale.** A listing is
  cached briefly so that stepping back into a directory you just left costs
  no round trip. A change made on the phone itself, or through Double
  Commander's other panel, shows up on the next refresh after that window
  rather than instantly.
- **Permission-denied directories look empty.** `adbd` answers a listing of
  a directory it cannot open (`/data`, `/data/data` on an unrooted phone)
  with a successful, empty result rather than an error, and the sync
  protocol offers nothing to tell that apart from a directory that really is
  empty. A path that does not exist, or that is a file rather than a
  directory, *is* reported properly.
- **Move (F6) to or from a local disk is refused by Double Commander
  itself.** Moving files from the device to a local folder, or from a local
  folder to the device, is rejected with "Function not implemented" before
  this plugin is asked to do anything — as is a move between two attached
  devices. Copying (F5) works in both directions, and rename/move *within*
  one device works normally; to move to or from the device, copy and then
  delete.
- **A narrow rename/move/copy race.** Refusing to overwrite an existing
  target (when "overwrite" isn't requested) is implemented as "check whether
  the target exists, then `mv` (or `cp`)" — there is a brief window between
  the check and the operation in which something else on the device could
  create the target, and it would then silently be overwritten. The device's
  shell offers no atomic "rename unless it exists" primitive to close that
  window with.

## The `FsSetTimeW` gotcha

Double Commander decides whether to copy file dates **at all** — in *both*
directions, uploads and downloads alike — based on whether the plugin
exports `FsSetTime` or `FsSetTimeW`. If neither is present, DC silently
strips the `caoCopyTime` flag (its internal "copy file date" flag) from
every copy operation, so dates would be lost even when *downloading*, a
direction this plugin's own code has no say in. See
[doublecmd/doublecmd#3051](https://github.com/doublecmd/doublecmd/issues/3051).

Because of this, `FsSetTimeW` must always be present in the exported symbol
table regardless of whether "change date" is ever actually invoked from the
UI. `scripts/check-exports.sh` and `tests/test_exports.cpp` both check for
it by name, and `nm -gU fsplugin.wfx64 | grep FsSetTimeW` is one of this
project's release checks.

## Testing

- `./run_tests.sh` — the full unit suite, which needs no device. Builds the
  plugin, compiles and runs every `tests/test_*.cpp`, and checks the
  exported symbol table. Ends with `ALL CHECKS PASSED`.

- `ADB_WFX_DEVICE_TESTS=1 ./tests/device_test.sh` — an opt-in end-to-end
  test against a real, attached Android device. It drives the plugin
  through `tests/device_driver.cpp`, a small standalone CLI that calls the
  same core the plugin itself uses. Covers device discovery, directory
  creation and listing, upload/download with a fixed mtime, unusual
  filenames (spaces, apostrophes, Cyrillic, and literal shell
  metacharacters), an on-device copy that keeps the source and its date,
  `settime`, cancelling a large transfer partway in both directions, and
  cleanup. Every on-device path it touches lives under
  `/sdcard/adb_wfx_test/`, and it removes that directory when it finishes.
  With no device attached it prints a clear message and exits with a
  distinct code (42).

- `./tests/bench.sh` — builds a local corpus (100 files, ~1.5–3 MB each) and
  times `adb push`/`adb pull` of the whole corpus against this plugin's own
  put/get, 5 runs each, reporting the median in seconds and MB/s plus the
  plugin/adb ratio. Also requires an attached device.

## Licensing and credits

This project is MIT-licensed (see `LICENSE`).

`wfxplugin.h` and `common.h` are vendored, unmodified, from the official
Double Commander SDK (`doublecmd/doublecmd`, `sdk/` directory); they carry
no copyright headers of their own.

The idea to build an ADB-based replacement came from observing the
modification-date limitations of an existing MTP plugin for Double Commander
(a separate, LGPL-licensed project) — **credited here as the source of the
idea only; no code from that project was used in this one.**
