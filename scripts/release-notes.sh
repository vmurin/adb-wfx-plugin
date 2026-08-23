#!/usr/bin/env bash
# Prints the release body for a tag: the matching CHANGELOG.md section plus
# the standing caveats every release has to carry.
set -euo pipefail

cd "$(dirname "$0")/.."

TAG="${1:?usage: scripts/release-notes.sh vX.Y.Z}"
VERSION="${TAG#v}"
VERSION="${VERSION%%-*}"

# Everything between "## [VERSION]" and the next "## [" heading.
awk -v ver="$VERSION" '
    $0 ~ "^## \\[" ver "\\]" { inside = 1; next }
    inside && /^## \[/       { exit }
    inside                   { print }
' CHANGELOG.md

cat <<'NOTES'

---

### Which file do I want?

| Archive | For |
| --- | --- |
| `adb-wfx-*-macos-universal.zip` | macOS, Apple silicon **and** Intel |
| `adb-wfx-*-linux-x86_64.zip` | Linux on x86_64 |
| `adb-wfx-*-linux-aarch64.zip` | Linux on ARM64 |

Verify a download against `SHA256SUMS`, then follow the **Installing** section of
the README. Copying the file into place is not enough on its own — the plugin
also has to be registered in Double Commander's own configuration.

**macOS:** clear the quarantine attribute after unzipping, or the plugin will not
load:

```sh
xattr -dr com.apple.quarantine fsplugin.wfx64
```

**Linux builds are untested on real hardware.** They compile and pass the full
unit suite in CI on both architectures, but nobody has yet run them against an
actual Android device inside Double Commander — unlike the macOS build, which is
hardware-confirmed on a Pixel 7. Reports either way are very welcome; please open
an issue.

**Windows / Total Commander** are not supported yet.
NOTES
