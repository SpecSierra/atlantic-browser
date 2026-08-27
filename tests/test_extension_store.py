#!/usr/bin/env python3
"""Checks the extension store's rules against the code that implements them.

Offline (always): the verdict tables are parsed out of WebExtensionStore.cpp
rather than copied, and the guard that keeps QZipReader out of the extractor.

Online (ATLANTIC_STORE_ONLINE=1): re-derives verdicts from what AMO currently
declares, so a rule that has drifted from the ecosystem shows up here rather
than on a device. Nothing is recommended by Atlantic any more -- the store is
search-only -- so this checks the rule, not a list.

Run: python3 tests/test_extension_store.py
     ATLANTIC_STORE_ONLINE=1 python3 tests/test_extension_store.py

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this file,
You can obtain one at http://mozilla.org/MPL/2.0/.
"""

import json
import os
import re
import sys
import unittest
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STORE_SOURCE = os.path.join(ROOT, "apps", "wpe", "WebExtensionStore.cpp")
MANAGER_SOURCE = os.path.join(ROOT, "apps", "wpe", "WebExtensionManager.cpp")

ONLINE = os.environ.get("ATLANTIC_STORE_ONLINE") == "1"


def api_tables():
    """Parse kBrokenApis / kPartialApis out of the C++ so there is one rule."""
    source = open(STORE_SOURCE, encoding="utf-8").read()
    tables = {}
    for name in ("kBrokenApis", "kPartialApis"):
        match = re.search(
            r"const char \*const %s\[\] = \{(.*?)\};" % name, source, re.S)
        assert match, "%s not found in WebExtensionStore.cpp" % name
        tables[name] = set(re.findall(r'"([^"]+)"', match.group(1)))
    return tables["kBrokenApis"], tables["kPartialApis"]


BROKEN_APIS, PARTIAL_APIS = api_tables()


def verdict_for(permissions):
    """Mirror of WebExtensionStore::verdictFor()."""
    broken, partial = [], []
    for permission in permissions:
        if "://" in permission or permission == "<all_urls>":
            continue
        if permission in BROKEN_APIS:
            broken.append(permission)
        elif permission in PARTIAL_APIS:
            partial.append(permission)
    if broken:
        return "broken", sorted(set(broken + partial))
    if partial:
        return "partial", sorted(set(partial))
    return "works", []


def fetch_addon(slug):
    request = urllib.request.Request(
        "https://addons.mozilla.org/api/v5/addons/addon/%s/?lang=en-US" % slug,
        headers={"User-Agent": "Atlantic/1.2 (catalog test)"})
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


def addon_permissions(addon):
    current = addon["current_version"]
    entry = current.get("file") or (current.get("files") or [{}])[0]
    return list(entry.get("permissions") or []) + list(entry.get("host_permissions") or [])


class ArchiveTest(unittest.TestCase):
    """Guards the reason WebExtensionArchive exists.

    AMO's signing pipeline emits streamed zips: general-purpose flag bit 3 is
    set on every entry, so the local file headers carry crc/sizes of zero and
    only the central directory has the real values. Qt 5.6's QZipReader reads
    the local header, extracts one file and fails the archive -- which is how
    "darkreader.xpi is not a readable extension archive" happened. If anyone
    ever "simplifies" the extractor back to QZipReader, these fail.
    """

    def test_manager_does_not_use_qzipreader(self):
        source = open(MANAGER_SOURCE, encoding="utf-8").read()
        self.assertNotIn("qzipreader", source.lower(),
                         "QZipReader cannot read AMO packages; use WebExtensionArchive")
        self.assertIn("WebExtensionArchive::extract", source)

    @unittest.skipUnless(ONLINE, "set ATLANTIC_STORE_ONLINE=1 to query AMO")
    def test_amo_packages_are_streamed_zips(self):
        import io
        import zipfile
        addon = fetch_addon("darkreader")
        entry = addon["current_version"]["file"]
        request = urllib.request.Request(
            entry["url"], headers={"User-Agent": "Atlantic/1.2 (catalog test)"})
        with urllib.request.urlopen(request, timeout=90) as response:
            blob = response.read()

        archive = zipfile.ZipFile(io.BytesIO(blob))
        infos = archive.infolist()
        self.assertGreater(len(infos), 1)
        # Bit 3 set everywhere is the hazard; if AMO ever stops doing this the
        # test should be revisited, not deleted.
        self.assertTrue(all(info.flag_bits & 0x08 for info in infos),
                        "expected every entry to carry a data descriptor")
        # And the local headers really are zeroed, which is what breaks a
        # local-header-based reader.
        import struct
        first = infos[0]
        crc, csize, usize = struct.unpack(
            "<III", blob[first.header_offset + 14:first.header_offset + 26])
        self.assertEqual((crc, csize, usize), (0, 0, 0),
                         "local header should be zeroed for a streamed entry")
        self.assertNotEqual(first.CRC, 0, "central directory should hold the real CRC")


class VerdictTest(unittest.TestCase):
    """The rule itself, not a list of add-ons."""

    def test_host_patterns_never_count(self):
        verdict, reasons = verdict_for(["https://*/*", "<all_urls>", "storage"])
        self.assertEqual(verdict, "works")
        self.assertEqual(reasons, [])

    def test_webrequest_is_broken_and_says_why(self):
        verdict, reasons = verdict_for(["webRequest", "webRequestBlocking", "storage"])
        self.assertEqual(verdict, "broken")
        self.assertIn("webRequest", reasons)

    def test_implemented_apis_are_not_flagged(self):
        # These four were wired to the browser's own subsystems; a permission
        # left behind in the tables would warn about something that works.
        for permission in ("cookies", "history", "bookmarks", "downloads",
                           "contextMenus", "scripting"):
            verdict, _ = verdict_for([permission])
            self.assertEqual(verdict, "works", permission)

    @unittest.skipUnless(ONLINE, "set ATLANTIC_STORE_ONLINE=1 to query AMO")
    def test_ublock_origin_is_still_broken(self):
        # The canary for the tables drifting: uBO is webRequest-based, and the
        # day that stops being true the rule needs revisiting.
        verdict, _ = verdict_for(addon_permissions(fetch_addon("ublock-origin")))
        self.assertEqual(verdict, "broken")


if __name__ == "__main__":
    unittest.main(verbosity=2)
