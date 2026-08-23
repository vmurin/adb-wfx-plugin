# Security policy

## Reporting a vulnerability

Please report security issues privately through GitHub's
[private vulnerability reporting](https://github.com/vmurin/adb-wfx-plugin/security/advisories/new)
rather than in a public issue. You can expect an acknowledgement within a few
days.

## What this plugin does, and what that implies

It helps to know the shape of the thing before deciding whether something is a
vulnerability.

- **It runs inside Double Commander**, as a shared library loaded into that
  process. A bug here can crash the file manager or corrupt a file being copied.
- **It talks to the local `adb` server only**, over TCP to `127.0.0.1:5037`
  (or `$ANDROID_ADB_SERVER_PORT`). It opens no listening socket, makes no
  outbound network connection, and sends nothing anywhere else.
- **It executes shell commands on the attached device.** `rm`, `mkdir`, `mv`,
  `cp`, `touch` and `stat` are issued over ADB's `shell:` service to carry out
  the operations Double Commander asks for. Every path that reaches that shell is
  single-quoted first (`shellQuote` in `adbutils.hpp`), and the unit suite pins
  the exact bytes put on the wire, including for filenames that look like shell
  metacharacters. A way to break out of that quoting is a genuine
  vulnerability — please report it.
- **It starts the `adb` server** by executing the `adb` binary once, at plugin
  init. The binary is located through `$ADB_PATH`, `PATH`, `$ANDROID_HOME`,
  `$ANDROID_SDK_ROOT` and a list of standard install locations. Anyone who can
  write to those locations or set those variables can already run code as you;
  that is the same trust boundary as any other tool that shells out.
- **It requires a device with USB debugging enabled**, authorised by you on the
  phone itself. It cannot grant itself access to a device you have not approved.

## Supported versions

The latest release is supported. Fixes go into a new release rather than being
backported.
