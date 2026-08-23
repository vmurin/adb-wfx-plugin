# adb-wfx

[![CI](https://github.com/vmurin/adb-wfx-plugin/actions/workflows/ci.yml/badge.svg)](https://github.com/vmurin/adb-wfx-plugin/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/vmurin/adb-wfx-plugin?sort=semver)](https://github.com/vmurin/adb-wfx-plugin/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

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

## Supported platforms

| Platform | Status |
| --- | --- |
| macOS, Apple silicon and Intel (universal binary) | Supported — hardware-confirmed on a Pixel 7 |
| Linux x86_64 | Builds and passes the full unit suite in CI — **untested on real hardware** |
| Linux aarch64 | Builds and passes the full unit suite in CI — **untested on real hardware** |
| Windows / Total Commander | Not yet — see [Windows and Total Commander](#windows-and-total-commander) |

> **The Linux builds are untested on real hardware.** They compile cleanly and
> pass all 900+ unit checks in CI on both architectures, but nobody has yet run
> them against an actual Android device inside Double Commander — unlike the
> macOS build, which is verified end to end on a real phone. Nothing is known to
> be wrong with them; they simply have not been tried. If you run one, please
> [open an issue](https://github.com/vmurin/adb-wfx-plugin/issues) saying whether
> it worked. Reports of success are as useful as reports of failure.

You will also need `adb` installed. The plugin finds it through `$ADB_PATH`,
your `PATH`, `$ANDROID_HOME` / `$ANDROID_SDK_ROOT`, and the usual Homebrew,
Android Studio and distribution package locations.

## Installing

Copying the plugin into place is **only half of the install**. Double Commander
does not scan for plugins — it loads the ones listed in its own configuration,
so step 4 below is required, not a fallback.

**1. Get the plugin.** Download the archive for your platform from
[Releases](https://github.com/vmurin/adb-wfx-plugin/releases/latest) and unzip
it, or build from source (see [Building](#building)).

**2. macOS only — clear the quarantine attribute.** Anything downloaded from the
internet is quarantined, and macOS will not load a quarantined library into
Double Commander:

```sh
xattr -dr com.apple.quarantine fsplugin.wfx64
```

**3. Put the file in place.**

```sh
./install.sh          # from an unpacked release archive
./scripts/install.sh  # from a source checkout
```

It copies `fsplugin.wfx64` into Double Commander's own configuration directory —
`~/Library/Preferences/doublecmd/plugins/wfx/adb/` on macOS,
`~/.config/doublecmd/plugins/wfx/adb/` on Linux — creating that subdirectory if
needed and backing up any file already there first. It writes nowhere else.

Installing into the configuration directory rather than into the application
itself is deliberate: a plugin inside `Double Commander.app` (or `/usr/lib`) is
wiped by the next update, and on macOS writing into the bundle invalidates its
code signature.

**4. Register it in Double Commander. This step is mandatory.**

1. **Configuration → Options… → Plugins → WFX plugins**
2. **Add**, and select the file that step 3 reported, e.g.
   `~/Library/Preferences/doublecmd/plugins/wfx/adb/fsplugin.wfx64`
3. Name it **`ADB`**, confirm with **OK**, then **Apply**.

**5. Quit Double Commander completely and start it again.**

**6. Open it.** **Commands → Open VFS list**, then choose **ADB**.

**7. On the phone**, enable USB debugging and authorise this computer when it
prompts.

Devices then appear as directories under the `ADB` root, named
`<model> (<serial>)` — for example `/<model> (<serial>)/sdcard/...`.

### Checking that the registration took

Double Commander records registered plugins in `doublecmd.xml`
(`~/Library/Preferences/doublecmd/doublecmd.xml` on macOS,
`~/.config/doublecmd/doublecmd.xml` on Linux). After step 4 it should contain:

```xml
<WfxPlugins>
  <WfxPlugin Enabled="True">
    <Name>ADB</Name>
    <Path>%DC_CONFIG_PATH%/plugins/wfx/adb/fsplugin.wfx64</Path>
  </WfxPlugin>
</WfxPlugins>
```

`./scripts/install.sh --print-xml` prints that block if you would rather paste it
in by hand. Edit the file only while Double Commander is closed — it rewrites
`doublecmd.xml` on exit and will overwrite your change.

### If something does not work

| Symptom | Cause |
| --- | --- |
| No `ADB` entry in the VFS list | Step 4 was skipped, or Double Commander was not restarted. |
| macOS: the plugin fails to load | The quarantine attribute is still set — step 2. |
| `ADB` opens but lists no devices | The device is not visible to `adb` itself. Check `adb devices`, the USB cable, and that you accepted the authorisation prompt on the phone. |
| "adb not found" | Set `ADB_PATH` to the full path of your `adb` binary. |
| Dates are not preserved on copy | Double Commander's "copy file date" option is off, or an older build without `FsSetTimeW` is still registered — see [the `FsSetTimeW` gotcha](#the-fssettimew-gotcha). |

## Building

Requires a C++17 compiler — `clang++` on macOS, `g++` or `clang++` on Linux.
There is nothing else to install.

```sh
./build.sh              # native build          -> fsplugin.wfx64
./build.sh --universal  # macOS arm64 + x86_64  -> fsplugin.wfx64
CXX=g++ ./build.sh      # pick the compiler
```

The result is a shared library exporting the 14 symbols Double Commander's WFX
interface requires (`FsInitW`, `FsFindFirstW`, …, including `FsSetTimeW` — see
[the `FsSetTimeW` gotcha](#the-fssettimew-gotcha) below) and nothing else;
`./scripts/check-exports.sh` asserts both halves of that.

`compile_mac.sh` and `compile_mac_universal.sh` still exist as thin wrappers
around `build.sh`.

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

## Windows and Total Commander

Neither is supported yet, and they are the same piece of work: Total Commander
runs only on Windows, and Double Commander on Windows uses the same plugin
binaries. The WFX API this plugin implements is Total Commander's own — no
Double-Commander-specific extension is used — so a Windows build should serve
both. What stands in the way is the host-side code: BSD sockets, `posix_spawn`
and POSIX file calls all need Windows equivalents, and the local-file paths need
to be handled as wide characters. See the
[Windows support issue](https://github.com/vmurin/adb-wfx-plugin/issues) for the
detailed analysis.

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
it by name, and it is one of this project's release checks.

## Testing

- `./run_tests.sh` — the full unit suite, which needs no device. Builds the
  plugin, compiles and runs every `tests/test_*.cpp`, and checks the
  exported symbol table. Ends with `ALL CHECKS PASSED`. This is what CI runs
  on macOS (arm64 and x86_64) and Linux (x86_64 and aarch64).

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

CI cannot attach a phone, so the last two are run by hand. The macOS build is
confirmed against a Pixel 7; see [Supported platforms](#supported-platforms) for
what that means for the Linux builds.

## Contributing

Bug reports, and especially reports from Linux users, are welcome. See
[CONTRIBUTING.md](CONTRIBUTING.md) for how to build, test and submit changes,
and [SECURITY.md](SECURITY.md) for how to report a security issue.

## Licensing and credits

This project is MIT-licensed (see `LICENSE`).

`wfxplugin.h` and `common.h` are vendored, unmodified, from the official
Double Commander SDK (`doublecmd/doublecmd`, `sdk/` directory); they carry
no copyright headers of their own.

The idea to build an ADB-based replacement came from observing the
modification-date limitations of an existing MTP plugin for Double Commander
(a separate, LGPL-licensed project) — **credited here as the source of the
idea only; no code from that project was used in this one.**
