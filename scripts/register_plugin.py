#!/usr/bin/env python3
"""Register the ADB WFX plugin in Double Commander's doublecmd.xml.

    register_plugin.py <doublecmd.xml> <path-value>

<path-value> is what goes into <Path>, normally

    %DC_CONFIG_PATH%/plugins/wfx/adb/fsplugin.wfx64

Idempotent: an entry this plugin already owns is updated in place rather than
duplicated, so re-running an install never grows the plugin list.

Ownership is decided by <Name> == "ADB" or by a <Path> ending in
plugins/wfx/adb/fsplugin.wfx64 -- never by the file name alone. Double
Commander's own MTP plugin is *also* called fsplugin.wfx64, and matching on
the basename would rewrite the user's MTP registration.

Double Commander rewrites doublecmd.xml when it exits, so it must not be
running while this script works. Checking that is install.sh's job.

The whole file is re-serialised, which normalises a few cosmetic things
ElementTree does differently from DC (self-closing tags, quoting in the XML
declaration). Those are undone below so the diff stays limited to the plugin
entry. Untouched elements keep their original whitespace, because ElementTree
parses indentation as text/tail nodes and writes it back unchanged.
"""
import datetime
import os
import shutil
import sys
import xml.etree.ElementTree as ET

NAME = "ADB"
PATH_SUFFIX = "plugins/wfx/adb/fsplugin.wfx64"
STEP = "  "  # DC indents with two spaces per level.

DEPTH_PLUGINS = 1  # <doublecmd> -> <Plugins>
DEPTH_WFXPLUGINS = 2  # -> <WfxPlugins>
DEPTH_ENTRY = 3  # -> <WfxPlugin>


def owns(entry, path_value):
    """True if this <WfxPlugin> is ours, by name or by installed location."""
    name = entry.find("Name")
    if name is not None and (name.text or "").strip() == NAME:
        return True
    path = entry.find("Path")
    if path is not None and (path.text or "").strip():
        text = path.text.strip().lower()
        return text.endswith(PATH_SUFFIX) or text == path_value.lower()
    return False


def append_child(parent, child, depth):
    """Append `child` to `parent`, indented as a level-`depth` element."""
    own_indent = "\n" + STEP * depth
    parent_indent = "\n" + STEP * (depth - 1)

    if parent.text is None or "\n" not in parent.text:
        parent.text = own_indent
    kids = list(parent)
    if kids:
        # The previous last child is no longer last, so it gets sibling
        # indentation instead of the closing tag's.
        kids[-1].tail = own_indent
    child.tail = parent_indent
    parent.append(child)


def build_entry(path_value):
    entry = ET.Element("WfxPlugin", {"Enabled": "True"})
    inner = "\n" + STEP * (DEPTH_ENTRY + 1)
    entry.text = inner
    name = ET.SubElement(entry, "Name")
    name.text = NAME
    name.tail = inner
    path = ET.SubElement(entry, "Path")
    path.text = path_value
    path.tail = "\n" + STEP * DEPTH_ENTRY
    return entry


def find_or_create(parent, tag, depth):
    found = parent.find(tag)
    if found is not None:
        return found
    created = ET.Element(tag)
    append_child(parent, created, depth)
    return created


def serialise(root):
    body = ET.tostring(root, encoding="unicode")
    # ElementTree writes `<Tag />` and single-quoted declarations; DC writes
    # `<Tag/>` and double quotes. Neither escapes a bare `>` into element
    # text or attribute values, so this substring cannot occur in content.
    body = body.replace(" />", "/>")
    text = '<?xml version="1.0" encoding="UTF-8"?>\n' + body
    if not text.endswith("\n"):
        text += "\n"
    return text


def main(config_path, path_value):
    # Parse before touching anything: a config we cannot read is a config we
    # must leave exactly as it is, without even leaving a backup behind.
    try:
        tree = ET.parse(config_path)
    except (ET.ParseError, OSError) as exc:
        sys.exit("register_plugin.py: cannot read %s: %s" % (config_path, exc))

    root = tree.getroot()
    plugins = find_or_create(root, "Plugins", DEPTH_PLUGINS)
    wfx = find_or_create(plugins, "WfxPlugins", DEPTH_WFXPLUGINS)

    existing = [el for el in list(wfx) if owns(el, path_value)]
    action = "updated" if existing else "added"
    for el in existing:
        wfx.remove(el)
    append_child(wfx, build_entry(path_value), DEPTH_ENTRY)

    backup = "%s.bak-%s" % (config_path, datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    shutil.copy2(config_path, backup)

    # Write through a temporary file in the same directory: a crash or a full
    # disk halfway through must not leave the user with a truncated config.
    temp = config_path + ".tmp-register"
    try:
        with open(temp, "w", encoding="utf-8") as handle:
            handle.write(serialise(root))
        os.replace(temp, config_path)
    except OSError as exc:
        if os.path.exists(temp):
            os.unlink(temp)
        sys.exit("register_plugin.py: cannot write %s: %s" % (config_path, exc))

    print("   backup:     %s" % backup)
    print("   %s: %s -> %s" % (action.ljust(7), NAME, path_value))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: register_plugin.py <doublecmd.xml> <path-value>")
    main(sys.argv[1], sys.argv[2])
