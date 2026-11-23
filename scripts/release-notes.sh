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

**Linux:** the release binaries need glibc 2.34 or newer (x86_64) and 2.38 or
newer (aarch64); on an older distribution, build from source. So far one user
has confirmed a self-built x86_64 plugin on a real device; the release archives
themselves, and aarch64, are not yet confirmed. Reports either way are very
welcome: please file a Linux build report.

**Windows / Total Commander** are not supported yet.
NOTES
