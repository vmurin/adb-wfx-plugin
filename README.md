# adb-wfx: an ADB file-system plugin for Double Commander

A Double Commander [WFX file-system plugin](https://doublecmd.sourceforge.io/)
that browses, copies, renames, and deletes files on an Android device over
ADB (Android Debug Bridge), talking to the `adb` server's wire protocol
directly rather than shelling out to the `adb` command-line tool for every
operation.

## Why this exists

Double Commander already ships an MTP plugin for Android devices. This
project exists because of one specific limitation in that plugin: **it
cannot preserve a file's modification date.**

On a Pixel 7, MTP's `Date Modified` object property came back read-only in
all 28 storage formats probed; asking the device to set it
(`SetObjectPropValue`) answered `0xA80A ObjectProp Not Supported`; and the
`libmtp` library underneath stamps every uploaded file with `time(NULL)`
(the moment of upload) regardless of what the caller asked for. There is no
way, through MTP on this device, to make an uploaded or downloaded file's
timestamp mean anything other than "when the copy happened."

ADB's `sync` protocol has no such limitation — `SEND` carries the source's
mtime to the device, and a directory `LIST`/`STAT` reports each file's real
mtime back. This plugin exists to use that instead.

### Measured throughput: adb vs MTP

The following numbers come from the design spec's own measurements, using
the raw `adb` and `mtp` tools directly (**not** this plugin — this
plugin's own `tests/bench.sh` has not yet been run against a device; see
[Known limitations](#known-limitations)). Corpus: 100 files, 238 MB total,
median of 5 runs:

| Operation  | adb            | MTP            |
|------------|----------------|----------------|
| push/write | 7.05 s / 33.8 MB/s | 18.56 s / 12.8 MB/s |
| pull/read  | 7.40 s / 32.2 MB/s | 12.54 s / 19.0 MB/s |

This plugin is built on the `adb` sync protocol specifically to inherit
that throughput along with the correct mtimes. `tests/bench.sh` (see
below) measures this plugin's own put/get against the same `adb`
baseline once a device is available.

## Building

Requires `clang++` with C++17 support and no other dependencies (no
libmtp, no CMake, no third-party libraries — plain POSIX sockets and the
C++ standard library only).

```sh
./compile_mac.sh
```

produces `fsplugin.wfx64`, a Mach-O shared library exporting the 14
symbols Double Commander's WFX interface requires (`FsInitW`,
`FsFindFirstW`, ..., including `FsSetTimeW` — see
[the FsSetTimeW gotcha](#the-fssettimew-gotcha) below).

To run the unit test suite (no device required):

```sh
./run_tests.sh
```

## Installing

```sh
./scripts/install.sh
```

copies `fsplugin.wfx64` into Double Commander's own plugin directory
(`.../Double Commander.app/Contents/MacOS/plugins/wfx/adb/`), creating
that subdirectory if needed and backing up any file already there to
`fsplugin.wfx64.bak-<timestamp>` first. It never touches Double
Commander's other WFX plugin directories (`ftp/`, MTP-style plugins,
etc.) — it only ever creates or writes inside its own `adb/` subdirectory.

After installing, quit Double Commander completely and relaunch it. If
the ADB file system doesn't appear automatically, open **Configuration →
Plugins → WFX plugins**, click **Add/Configure**, and point it at the
installed `fsplugin.wfx64`.

Once connected, devices appear as directories under a virtual `ADB` root
(`/<model> (<serial>)/...`, e.g. `/Pixel_7 (27281FDH2008DM)/sdcard/...`).

## What works

- Browsing the device's filesystem (backed by `adb`'s sync `LIST`/`STAT`).
- Uploading and downloading files with the real modification time
  preserved in both directions.
- Delete, rename/move (within one device), create directory.
- Setting a file's modification date from Double Commander's own "change
  date" UI (`FsSetTimeW`).
- Filenames containing spaces, quotes, and non-ASCII (e.g. Cyrillic)
  characters, and filenames that happen to look like shell
  metacharacters (`$(...)`, `` `...` ``) — every path sent to the
  device's shell is single-quoted before it gets there.

## Known limitations

- **Sync protocol v1 only.** This plugin speaks the same wire protocol
  as the stock `adb` client, which has two hard limits baked into its
  packet format:
  - **32-bit mtime**: timestamps are a 32-bit count of seconds since the
    epoch, so dates past **January 19, 2038** cannot be represented (the
    "Y2038 problem").
  - **32-bit size**: file sizes are likewise 32-bit, so files **4 GiB or
    larger** cannot be transferred.
- **No APK install, no logcat, no shell UI.** This is a file-transfer
  plugin, not a device-management tool.
- **Global, unsynchronized state.** The plugin keeps one `AdbClient`, one
  directory-listing cache, and one `PluginCore` instance for its entire
  lifetime, with no locking. Double Commander's background/multi-threaded
  transfer modes call into a WFX plugin from more than one thread at
  once, and this plugin does not make that safe — only its normal,
  single-threaded copy/move/browse operations are supported.
- **A narrow rename/move race.** Refusing to overwrite an existing target
  (when "overwrite" isn't requested) is implemented as "check whether the
  target exists, then `mv`" — there is a brief window between the check
  and the move in which something else on the device could create the
  target, and the move would then silently overwrite it. The device's
  shell gives this plugin no atomic "rename unless it exists" primitive
  to close that window with.

## The `FsSetTimeW` gotcha

Double Commander decides whether to copy file dates **at all** — in
*both* directions, uploads and downloads alike — based on whether the
plugin exports `FsSetTime` or `FsSetTimeW`. If neither is present, DC
silently strips the "copy file date" flag from every copy operation, so
dates would be lost even when *downloading*, a direction this plugin's
own code has no say in. See
[doublecmd/doublecmd#3051](https://github.com/doublecmd/doublecmd/issues/3051).

Because of this, `FsSetTimeW` must always be present in the exported
symbol table regardless of whether "change date" is ever actually
invoked from the UI — `scripts/check-exports.sh` and
`tests/test_exports.cpp` both check for it by name, and `nm -gU
fsplugin.wfx64 | grep FsSetTimeW` is one of this project's release
checks.

## Testing

- `./run_tests.sh` — the full unit test suite (no device needed): builds
  the plugin, compiles and runs every `tests/test_*.cpp`, checks the
  exported symbol table, and checks the license text. Ends with `ALL
  CHECKS PASSED`.
- `ADB_WFX_DEVICE_TESTS=1 ./tests/device_test.sh` — an opt-in end-to-end
  test against a real, attached Android device. Drives the plugin
  through `tests/device_driver.cpp`, a small standalone CLI that calls
  the same `PluginCore` the plugin itself uses (never the DC GUI, and
  never `adb` for the operations under test). Covers device discovery,
  directory creation/listing, upload/download with a fixed mtime,
  unusual filenames (spaces, apostrophes, Cyrillic, and literal shell
  metacharacters), `settime`, cancelling a large transfer partway, and
  cleanup. Every on-device path it touches lives under
  `/sdcard/adb_wfx_test/`, and it removes that directory (and nothing
  else) when it finishes. If no device is attached, it prints a clear
  message and exits with a distinct code (42) rather than passing or
  failing silently.
- `./tests/bench.sh` — builds a local corpus (100 files, ~1.5–3 MB each)
  and times `adb push`/`adb pull` of the whole corpus against this
  plugin's own put/get (via `tests/device_driver.cpp`), 5 runs each,
  reporting the median in seconds and MB/s plus the plugin/adb ratio. It
  reports numbers; it does not fail the build over them. Also requires a
  device, and fails the same clearly-labelled way if none is attached.

## Licensing and credits

This project is MIT-licensed (see `LICENSE`).

`wfxplugin.h` and `common.h` are vendored, unmodified, from the official
Double Commander SDK (`doublecmd/doublecmd`, `sdk/` directory); they
carry no copyright headers of their own.

The idea to build an ADB-based replacement came from observing the
modification-date limitations of an existing MTP plugin for Double
Commander (a separate, LGPL-licensed project) — **credited here as the
source of the idea only; no code from that project was used in this
one.**
