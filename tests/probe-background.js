#!/usr/bin/env node
/*
 * Runs a real extension's background script against ONLY the globals Atlantic's
 * JSC background host provides, and reports the first thing it reaches for that
 * is not there.
 *
 * Why this exists: background scripts run in a bare JavaScriptCore context, not
 * a DOM, so anything they expect from a service-worker global has to be
 * polyfilled by hand in kBackgroundPreamble. Finding the gaps by installing on a
 * device costs a ~12 minute CI build per attempt; this costs a second, and it
 * found the two that mattered — LanguageTool dying on self.addEventListener and
 * Dark Reader on chrome.runtime.setUninstallURL, each of which killed the whole
 * script at its first top-level call.
 *
 * The sandbox is deliberately impoverished: it holds the ECMAScript built-ins
 * and nothing else, because that is what jsc_context_new() gives us. Do not add
 * conveniences to it — every global here must be one the preamble really
 * defines, or the probe stops telling the truth.
 *
 * Usage:
 *   node tests/probe-background.js <unpacked-extension-dir>
 *
 * Unpack an .xpi with any zip tool; it is an ordinary archive (see
 * WebExtensionArchive for why Qt's reader is not).
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

"use strict";

const fs = require("fs");
const path = require("path");
const vm = require("vm");

const HEADER = path.join(__dirname, "..", "apps", "wpe", "WebExtensionScripts.h");

function rawString(source, name) {
    const start = source.indexOf(name);
    if (start === -1) throw new Error(name + " not found in WebExtensionScripts.h");
    const open = source.indexOf('R"JS(', start);
    return source.substring(open + 5, source.indexOf(')JS"', open));
}

// Mirrors WebExtensionBackground's own resolution, background pages included:
// a page's <script src> list is extracted and run, its inline code ignored.
function backgroundScripts(extensionDir, manifest) {
    const background = manifest.background || {};
    const scripts = (background.scripts || []).slice();
    if (background.service_worker) scripts.push(background.service_worker);

    if (background.page) {
        const pagePath = path.join(extensionDir, background.page);
        if (fs.existsSync(pagePath)) {
            const html = fs.readFileSync(pagePath, "utf8");
            const tag = /<script[^>]*\ssrc\s*=\s*["']([^"']+)["']/gi;
            let match;
            while ((match = tag.exec(html)) !== null)
                scripts.push(path.join(path.dirname(background.page), match[1]));
        }
    }
    return scripts;
}

function makeContext(extensionDir, manifest, header) {
    // Exactly what a bare JSC global has. Nothing else.
    const sandbox = {
        Object, Array, Function, String, Number, Boolean, Symbol, Math, JSON, Date,
        RegExp, Error, TypeError, RangeError, SyntaxError, Promise, Map, Set, WeakMap,
        WeakSet, Proxy, Reflect, ArrayBuffer, DataView, Uint8Array, Int8Array,
        Uint16Array, Int16Array, Uint32Array, Int32Array, Float32Array, Float64Array,
        parseInt, parseFloat, isNaN, isFinite,
        encodeURIComponent, decodeURIComponent, encodeURI, decodeURI
    };
    sandbox.globalThis = sandbox;

    const posted = [];
    sandbox.__atlNative = (json) => posted.push(JSON.parse(json));
    vm.createContext(sandbox);

    vm.runInContext(rawString(header, "kBackgroundPreamble"), sandbox,
                    { filename: "kBackgroundPreamble" });
    vm.runInContext(
        rawString(header, "kApiShim")
            .replace("@@ATL_EXT_ID@@", JSON.stringify("probe"))
            .replace("@@ATL_MANIFEST@@", JSON.stringify(manifest))
            .replace("@@ATL_L10N@@", "{}")
            .replace("@@ATL_CONTEXT@@", JSON.stringify("background"))
            .replace("@@ATL_HANDLER@@", JSON.stringify("")),
        sandbox, { filename: "kApiShim" });

    return { sandbox, posted };
}

function main() {
    const extensionDir = process.argv[2];
    if (!extensionDir) {
        console.error("usage: node tests/probe-background.js <unpacked-extension-dir>");
        process.exit(2);
    }

    const manifest = JSON.parse(
        fs.readFileSync(path.join(extensionDir, "manifest.json"), "utf8"));
    const header = fs.readFileSync(HEADER, "utf8");
    const scripts = backgroundScripts(extensionDir, manifest);

    console.log("%s (manifest v%s)", manifest.name, manifest.manifest_version);
    if (scripts.length === 0) {
        console.log("  no background scripts — nothing to probe");
        return 0;
    }

    let failed = 0;
    for (const script of scripts) {
        const file = path.join(extensionDir, script);
        if (!fs.existsSync(file)) {
            console.log("  %s: MISSING from the package", script);
            failed++;
            continue;
        }

        const { sandbox, posted } = makeContext(extensionDir, manifest, header);
        try {
            vm.runInContext(fs.readFileSync(file, "utf8"), sandbox, { filename: script });
            console.log("  %s: ran to completion", script);
        } catch (error) {
            failed++;
            console.log("  %s: THREW %s", script, error.message);
            const frame = (error.stack || "").split("\n").find(l => l.includes(script));
            if (frame) console.log("      at %s", frame.trim());
        }

        const apis = new Set(posted.map(p => p.api));
        if (apis.size)
            console.log("      APIs used: %s", [...apis].sort().join(" "));
        posted.filter(p => p.api === "log" && p.args[0] !== "log")
              .slice(0, 3)
              .forEach(l => console.log("      [%s] %s", l.args[0],
                                        String(l.args[1]).slice(0, 140)));
    }
    return failed ? 1 : 0;
}

process.exit(main());
