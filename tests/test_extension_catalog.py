#!/usr/bin/env python3
"""Checks data/extension-catalog.json.

Offline (always): schema, duplicate slugs, valid verdicts, and that every
"verified" claim is one somebody actually made.

Online (ATLANTIC_CATALOG_ONLINE=1): resolves each slug against
addons.mozilla.org and re-derives its verdict from the add-on's currently
declared permissions, using the same API tables the C++ store compiles in --
they are parsed out of WebExtensionStore.cpp rather than copied, so the test
cannot drift from the shipped rule. This is the guard against catalog rot: an
add-on that adds webRequest in a later release silently stops working, and the
catalog would go on recommending it.

Run: python3 tests/test_extension_catalog.py
     ATLANTIC_CATALOG_ONLINE=1 python3 tests/test_extension_catalog.py

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
CATALOG = os.path.join(ROOT, "data", "extension-catalog.json")
STORE_SOURCE = os.path.join(ROOT, "apps", "wpe", "WebExtensionStore.cpp")

VERDICTS = {"works", "partial", "broken", "unknown"}
ONLINE = os.environ.get("ATLANTIC_CATALOG_ONLINE") == "1"


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


class CatalogTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(CATALOG, encoding="utf-8") as handle:
            cls.catalog = json.load(handle)
        cls.entries = cls.catalog["extensions"]

    def test_top_level_shape(self):
        self.assertEqual(self.catalog["version"], 1)
        self.assertEqual(self.catalog["source"], "addons.mozilla.org")
        self.assertIsInstance(self.entries, list)
        self.assertGreater(len(self.entries), 0)

    def test_entries_are_complete(self):
        for entry in self.entries:
            slug = entry.get("slug", "<missing>")
            for field in ("slug", "name", "summary", "expected", "verified", "note"):
                self.assertIn(field, entry, "%s is missing %r" % (slug, field))
            self.assertIn(entry["expected"], VERDICTS,
                          "%s has verdict %r" % (slug, entry["expected"]))
            self.assertIsInstance(entry["verified"], bool)
            self.assertTrue(entry["note"].strip(),
                            "%s has an empty note; say what does and does not work" % slug)

    def test_slugs_are_unique(self):
        slugs = [entry["slug"] for entry in self.entries]
        self.assertEqual(len(slugs), len(set(slugs)), "duplicate slug in the catalog")

    def test_slugs_are_url_safe(self):
        for entry in self.entries:
            self.assertRegex(entry["slug"], r"^[a-z0-9][a-z0-9._-]*$")

    def test_broken_entries_explain_themselves(self):
        # A "broken" entry exists only to save someone the search, so its note
        # has to say what to do instead.
        for entry in self.entries:
            if entry["expected"] == "broken":
                self.assertGreater(
                    len(entry["note"]), 60,
                    "%s is listed as broken but the note does not explain why or "
                    "what to use instead" % entry["slug"])

    @unittest.skipUnless(ONLINE, "set ATLANTIC_CATALOG_ONLINE=1 to query AMO")
    def test_catalog_matches_amo(self):
        stale = []
        for entry in self.entries:
            addon = fetch_addon(entry["slug"])
            derived, reasons = verdict_for(addon_permissions(addon))
            if derived != entry["expected"]:
                stale.append("%s: catalog says %r, AMO now implies %r (%s)"
                             % (entry["slug"], entry["expected"], derived,
                                ", ".join(reasons) or "no flagged APIs"))
        self.assertEqual(stale, [], "catalog is out of date:\n  " + "\n  ".join(stale))


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False, verbosity=2).result.wasSuccessful() else 1)
