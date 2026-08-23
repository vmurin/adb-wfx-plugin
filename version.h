// The single source of truth for this plugin's version.
//
// Everything else derives from it: the release workflow refuses to publish a
// tag that disagrees with ADB_WFX_VERSION, the packaged pluginst.inf gets it
// substituted in, and `device_driver --version` prints it. Bump it here and
// in CHANGELOG.md together -- CI checks that the two agree.
#ifndef ADB_WFX_VERSION_H
#define ADB_WFX_VERSION_H

#define ADB_WFX_VERSION "1.0.0"

#endif // ADB_WFX_VERSION_H
