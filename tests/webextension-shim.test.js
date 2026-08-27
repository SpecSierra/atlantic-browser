#!/usr/bin/env node
/*
 * Host-side test for the browser.* shim in apps/wpe/WebExtensionScripts.h.
 *
 * The shim is by far the largest piece of the WebExtension support and the one
 * that cannot be checked by the compiler, so it is exercised here against a
 * stub of the C++ bridge: every message the shim posts is captured, and replies
 * and events are fed back the way WebExtensionManager would.
 *
 * Run: node tests/webextension-shim.test.js
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

"use strict";

const fs = require("fs");
const path = require("path");
const vm = require("vm");
const assert = require("assert");

const HEADER = path.join(__dirname, "..", "apps", "wpe", "WebExtensionScripts.h");

// Pulls one R"JS( ... )JS" raw string literal out of the header by its
// `kName = R"JS(` introducer, so the test always runs the shipped source.
function extractRawString(source, name) {
    const start = source.indexOf(name);
    assert.ok(start !== -1, `${name} not found in WebExtensionScripts.h`);
    const open = source.indexOf('R"JS(', start);
    assert.ok(open !== -1, `${name} is not a R"JS( ... )JS" literal`);
    const bodyStart = open + 'R"JS('.length;
    const bodyEnd = source.indexOf(')JS"', bodyStart);
    assert.ok(bodyEnd !== -1, `${name} literal is unterminated`);
    return source.substring(bodyStart, bodyEnd);
}

const headerSource = fs.readFileSync(HEADER, "utf8");
const shimSource = extractRawString(headerSource, "kApiShim");
const preambleSource = extractRawString(headerSource, "kBackgroundPreamble");

const MANIFEST = {
    manifest_version: 2,
    name: "Test Extension",
    version: "1.0",
    permissions: ["storage", "tabs"]
};

const L10N = {
    greeting: { message: "Hello $WHO$ and $2", placeholders: { WHO: { content: "$1" } } },
    plain: { message: "No substitutions" }
};

// --- the stub C++ side -------------------------------------------------------

function makeSandbox(context) {
    const posted = [];
    const sandbox = {
        console,
        setTimeout,
        clearTimeout,
        Promise,
        JSON,
        Object,
        Array,
        Error,
        String,
        Number,
        Date,
        RegExp,
        navigator: { language: "en-GB" }
    };
    sandbox.globalThis = sandbox;

    const post = (json) => posted.push(JSON.parse(json));
    if (context === "background") {
        sandbox.__atlNative = post;
    } else {
        sandbox.webkit = { messageHandlers: { atlExt_test: { postMessage: post } } };
    }

    const script = shimSource
        .replace("@@ATL_EXT_ID@@", JSON.stringify("test"))
        .replace("@@ATL_MANIFEST@@", JSON.stringify(MANIFEST))
        .replace("@@ATL_L10N@@", JSON.stringify(L10N))
        .replace("@@ATL_CONTEXT@@", JSON.stringify(context))
        .replace("@@ATL_HANDLER@@", JSON.stringify("atlExt_test"));

    vm.createContext(sandbox);
    vm.runInContext(script, sandbox, { filename: "WebExtensionScripts.h:kApiShim" });

    return {
        sandbox,
        posted,
        browser: sandbox.browser,
        // Answers the most recent outstanding call, the way reply() does.
        reply(seq, value, error) {
            sandbox.__atlExtBridge.dispatch(JSON.stringify(
                error ? { type: "reply", seq, ok: false, error }
                      : { type: "reply", seq, ok: true, value }));
        },
        event(name, args) {
            sandbox.__atlExtBridge.dispatch(
                JSON.stringify({ type: "event", name, args }));
        },
        takeCalls(api) {
            const matching = posted.filter((message) => message.api === api);
            return matching;
        },
        lastCall() {
            return posted[posted.length - 1];
        }
    };
}

// --- tests -------------------------------------------------------------------

const tests = [];
function test(name, fn) { tests.push({ name, fn }); }

test("runtime.getURL builds an extension URL and strips leading slashes", () => {
    const { browser } = makeSandbox("content");
    assert.strictEqual(browser.runtime.getURL("popup.html"),
                       "atlantic-extension://test/popup.html");
    assert.strictEqual(browser.runtime.getURL("/icons/x.png"),
                       "atlantic-extension://test/icons/x.png");
});

test("runtime.getManifest returns a copy, not the live object", () => {
    const { browser } = makeSandbox("content");
    const manifest = browser.runtime.getManifest();
    assert.strictEqual(manifest.name, "Test Extension");
    manifest.name = "mutated";
    assert.strictEqual(browser.runtime.getManifest().name, "Test Extension");
});

test("chrome is the same API object as browser", () => {
    const { sandbox } = makeSandbox("content");
    assert.strictEqual(sandbox.chrome, sandbox.browser);
});

test("i18n.getMessage resolves named placeholders then $n", () => {
    const { browser } = makeSandbox("content");
    assert.strictEqual(browser.i18n.getMessage("greeting", ["Bob", "Alice"]),
                       "Hello Bob and Alice");
    assert.strictEqual(browser.i18n.getMessage("plain"), "No substitutions");
    assert.strictEqual(browser.i18n.getMessage("missing"), "");
    assert.strictEqual(browser.i18n.getMessage("@@extension_id"), "test");
});

test("storage.get posts the area and keys, and resolves a promise", async () => {
    const harness = makeSandbox("content");
    const promise = harness.browser.storage.local.get(["a", "b"]);
    const call = harness.lastCall();
    assert.strictEqual(call.api, "storage.get");
    assert.deepStrictEqual(call.args, ["local", ["a", "b"]]);
    harness.reply(call.seq, { a: 1 });
    assert.deepStrictEqual(await promise, { a: 1 });
});

test("storage.get with no arguments asks for everything", () => {
    const harness = makeSandbox("content");
    harness.browser.storage.local.get();
    assert.deepStrictEqual(harness.lastCall().args, ["local", null]);
});

test("a trailing callback switches to callback style and returns undefined", (done) => {
    const harness = makeSandbox("content");
    const result = harness.browser.storage.local.get("k", (value) => {
        assert.deepStrictEqual(value, { k: "v" });
        done();
    });
    assert.strictEqual(result, undefined);
    const call = harness.lastCall();
    assert.deepStrictEqual(call.args, ["local", "k"]);
    harness.reply(call.seq, { k: "v" });
});

test("a failed call sets runtime.lastError for the duration of the callback", (done) => {
    const harness = makeSandbox("content");
    harness.browser.storage.local.get("k", () => {
        assert.ok(harness.browser.runtime.lastError);
        assert.strictEqual(harness.browser.runtime.lastError.message, "boom");
        done();
    });
    harness.reply(harness.lastCall().seq, undefined, "boom");
});

test("an unsupported API rejects rather than silently doing nothing", async () => {
    const { browser } = makeSandbox("content");
    await assert.rejects(() => browser.cookies.get({ name: "x" }),
                         /not supported by Atlantic/);
});

test("an unsupported API still accepts listeners", () => {
    const { browser } = makeSandbox("content");
    // Extensions register these at top level; throwing would kill the script.
    browser.webRequest.onBeforeRequest.addListener(() => {});
    assert.ok(browser.webRequest.onBeforeRequest.hasListeners());
});

test("runtime.sendMessage resolves with the reply", async () => {
    const harness = makeSandbox("content");
    const promise = harness.browser.runtime.sendMessage({ hello: true });
    const call = harness.lastCall();
    assert.strictEqual(call.api, "runtime.sendMessage");
    assert.deepStrictEqual(call.args, [null, { hello: true }]);
    harness.reply(call.seq, { pong: 1 });
    assert.deepStrictEqual(await promise, { pong: 1 });
});

test("runtime.sendMessage accepts an explicit extension id first", () => {
    const harness = makeSandbox("content");
    harness.browser.runtime.sendMessage("other", { hello: true });
    assert.deepStrictEqual(harness.lastCall().args, ["other", { hello: true }]);
});

test("onMessage: a listener returning a value answers synchronously", () => {
    const harness = makeSandbox("content");
    harness.browser.runtime.onMessage.addListener(() => "answer");
    harness.event("runtime.onMessage", [{ q: 1 }, { id: "test" }, "tok1"]);
    const response = harness.takeCalls("runtime.onMessageResponse").pop();
    assert.deepStrictEqual(response.args, ["tok1", true, "answer"]);
});

test("onMessage: no listeners reports that nobody answered", () => {
    const harness = makeSandbox("content");
    harness.event("runtime.onMessage", [{ q: 1 }, { id: "test" }, "tok2"]);
    const response = harness.takeCalls("runtime.onMessageResponse").pop();
    assert.deepStrictEqual(response.args, ["tok2", false, null]);
});

test("onMessage: returning true defers to a later sendResponse", () => {
    const harness = makeSandbox("content");
    let saved = null;
    harness.browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
        saved = sendResponse;
        return true;
    });
    harness.event("runtime.onMessage", [{ q: 1 }, { id: "test" }, "tok3"]);
    assert.strictEqual(harness.takeCalls("runtime.onMessageResponse").length, 0,
                       "must not answer before sendResponse is called");
    saved({ later: true });
    assert.deepStrictEqual(harness.takeCalls("runtime.onMessageResponse").pop().args,
                           ["tok3", true, { later: true }]);
});

test("onMessage: a returned promise answers when it settles", async () => {
    const harness = makeSandbox("content");
    harness.browser.runtime.onMessage.addListener(() => Promise.resolve("async"));
    harness.event("runtime.onMessage", [{}, { id: "test" }, "tok4"]);
    await Promise.resolve();
    await Promise.resolve();
    assert.deepStrictEqual(harness.takeCalls("runtime.onMessageResponse").pop().args,
                           ["tok4", true, "async"]);
});

test("onMessage: sendResponse only answers once", () => {
    const harness = makeSandbox("content");
    harness.browser.runtime.onMessage.addListener((m, s, sendResponse) => {
        sendResponse("first");
        sendResponse("second");
    });
    harness.event("runtime.onMessage", [{}, { id: "test" }, "tok5"]);
    assert.strictEqual(harness.takeCalls("runtime.onMessageResponse").length, 1);
});

test("onMessage: a throwing listener does not stop the others", () => {
    const harness = makeSandbox("content");
    harness.browser.runtime.onMessage.addListener(() => { throw new Error("bad"); });
    harness.browser.runtime.onMessage.addListener(() => "survived");
    harness.event("runtime.onMessage", [{}, { id: "test" }, "tok6"]);
    assert.deepStrictEqual(harness.takeCalls("runtime.onMessageResponse").pop().args,
                           ["tok6", true, "survived"]);
});

test("ports: connect announces itself and delivers inbound messages", () => {
    const harness = makeSandbox("content");
    const port = harness.browser.runtime.connect({ name: "channel" });
    const connect = harness.takeCalls("runtime.connect").pop();
    assert.strictEqual(connect.args[1], "channel");
    const portId = connect.args[0];

    const received = [];
    port.onMessage.addListener((message) => received.push(message));
    harness.event("runtime.portMessage", [portId, { tick: 1 }]);
    assert.deepStrictEqual(received, [{ tick: 1 }]);

    port.postMessage({ tock: 2 });
    assert.deepStrictEqual(harness.takeCalls("runtime.portMessage").pop().args,
                           [portId, { tock: 2 }]);
});

test("ports: an inbound connection fires onConnect with a usable port", () => {
    const harness = makeSandbox("content");
    let port = null;
    harness.browser.runtime.onConnect.addListener((p) => { port = p; });
    harness.event("runtime.onConnect", ["p1", "named", { id: "test" }]);
    assert.ok(port, "onConnect did not fire");
    assert.strictEqual(port.name, "named");

    let disconnected = false;
    port.onDisconnect.addListener(() => { disconnected = true; });
    harness.event("runtime.portDisconnect", ["p1"]);
    assert.strictEqual(disconnected, true);
});

test("storage.onChanged is delivered with changes and area", () => {
    const harness = makeSandbox("content");
    let seen = null;
    harness.browser.storage.onChanged.addListener((changes, area) => {
        seen = { changes, area };
    });
    harness.event("storage.onChanged", [{ k: { newValue: 1 } }, "local"]);
    assert.deepStrictEqual(seen, { changes: { k: { newValue: 1 } }, area: "local" });
});

test("tabs events fan out to the matching Event object", () => {
    const harness = makeSandbox("background");
    const seen = [];
    harness.browser.tabs.onUpdated.addListener((tabId, changeInfo) => {
        seen.push([tabId, changeInfo.status]);
    });
    harness.event("tabs.onUpdated", [7, { status: "complete" }, { id: 7 }]);
    assert.deepStrictEqual(seen, [[7, "complete"]]);
});

test("removeListener actually removes", () => {
    const harness = makeSandbox("content");
    let calls = 0;
    const listener = () => { calls++; };
    harness.browser.tabs.onRemoved.addListener(listener);
    harness.event("tabs.onRemoved", [1, {}]);
    harness.browser.tabs.onRemoved.removeListener(listener);
    harness.event("tabs.onRemoved", [1, {}]);
    assert.strictEqual(calls, 1);
});

test("the background context posts through __atlNative", () => {
    const harness = makeSandbox("background");
    harness.browser.storage.local.set({ a: 1 });
    assert.strictEqual(harness.lastCall().api, "storage.set");
});

test("alarms fire locally without a round trip", (done) => {
    const harness = makeSandbox("background");
    harness.browser.alarms.onAlarm.addListener((alarm) => {
        assert.strictEqual(alarm.name, "tick");
        done();
    });
    harness.browser.alarms.create("tick", { delayInMinutes: 0 });
    assert.strictEqual(harness.takeCalls("timer.set").length, 0,
                       "alarms must not reach the native bridge directly");
});

// --- background preamble -----------------------------------------------------
//
// The preamble is what a background script actually lands in: JSC gives us a
// bare global with no timers, console, or network. Nothing else exercises it,
// so it gets its own bare sandbox here.

function makePreambleSandbox() {
    const posted = [];
    const sandbox = {
        Promise, JSON, Object, Array, Error, String, Number, Date, RegExp, Math,
        __atlNative: (json) => posted.push(JSON.parse(json))
    };
    sandbox.globalThis = sandbox;
    vm.createContext(sandbox);
    vm.runInContext(preambleSource, sandbox,
                    { filename: "WebExtensionScripts.h:kBackgroundPreamble" });
    return { sandbox, posted };
}

test("preamble: setTimeout registers a native timer and fires through __atlFireTimer", () => {
    const { sandbox, posted } = makePreambleSandbox();
    let fired = 0;
    const id = sandbox.setTimeout(() => { fired++; }, 25);
    const call = posted.pop();
    assert.strictEqual(call.api, "timer.set");
    assert.deepStrictEqual(call.args, [id, 25, false]);

    sandbox.__atlFireTimer(id);
    assert.strictEqual(fired, 1);
    // A one-shot timer must not fire twice.
    sandbox.__atlFireTimer(id);
    assert.strictEqual(fired, 1);
});

test("preamble: clearTimeout cancels natively and locally", () => {
    const { sandbox, posted } = makePreambleSandbox();
    let fired = 0;
    const id = sandbox.setInterval(() => { fired++; }, 5);
    sandbox.clearTimeout(id);
    assert.strictEqual(posted.pop().api, "timer.clear");
    sandbox.__atlFireTimer(id);
    assert.strictEqual(fired, 0);
});

test("preamble: console routes to the log API", () => {
    const { sandbox, posted } = makePreambleSandbox();
    sandbox.console.warn("careful", { n: 1 });
    assert.deepStrictEqual(posted.pop().args, ["warn", 'careful {"n":1}']);
});

test("preamble: fetch resolves through __atlFetchDone", async () => {
    const { sandbox, posted } = makePreambleSandbox();
    const promise = sandbox.fetch("https://example.com/list.txt");
    const call = posted.pop();
    assert.strictEqual(call.api, "net.fetch");
    const [requestId, url, method] = call.args;
    assert.strictEqual(url, "https://example.com/list.txt");
    assert.strictEqual(method, "GET");

    sandbox.__atlFetchDone(requestId, true, 200, "OK",
                           { "Content-Type": "text/plain" }, "body text", null);
    const response = await promise;
    assert.strictEqual(response.ok, true);
    assert.strictEqual(response.status, 200);
    assert.strictEqual(response.headers.get("content-type"), "text/plain");
    assert.strictEqual(await response.text(), "body text");
});

test("preamble: a failed fetch rejects", async () => {
    const { sandbox, posted } = makePreambleSandbox();
    const promise = sandbox.fetch("https://example.com/");
    sandbox.__atlFetchDone(posted.pop().args[0], false, 0, "", {}, null, "host unreachable");
    await assert.rejects(() => promise, /host unreachable/);
});

test("preamble: atob and btoa round-trip", () => {
    const { sandbox } = makePreambleSandbox();
    assert.strictEqual(sandbox.btoa("Atlantic"), "QXRsYW50aWM=");
    assert.strictEqual(sandbox.atob("QXRsYW50aWM="), "Atlantic");
});

test("preamble: XMLHttpRequest goes through fetch", (done) => {
    const { sandbox, posted } = makePreambleSandbox();
    const request = new sandbox.XMLHttpRequest();
    request.open("GET", "https://example.com/x");
    request.onload = () => {
        assert.strictEqual(request.status, 200);
        assert.strictEqual(request.responseText, "hi");
        done();
    };
    request.send();
    sandbox.__atlFetchDone(posted.pop().args[0], true, 200, "OK", {}, "hi", null);
});

// --- runner ------------------------------------------------------------------

(async function run() {
    let failed = 0;
    for (const { name, fn } of tests) {
        try {
            if (fn.length > 0) {
                await new Promise((resolve, reject) => {
                    const timer = setTimeout(() => reject(new Error("timed out")), 2000);
                    fn((error) => {
                        clearTimeout(timer);
                        error ? reject(error) : resolve();
                    });
                });
            } else {
                await fn();
            }
            console.log(`  ok   ${name}`);
        } catch (error) {
            failed++;
            console.log(`  FAIL ${name}\n       ${error && error.message}`);
        }
    }
    console.log(`\n${tests.length - failed}/${tests.length} passed`);
    process.exit(failed ? 1 : 0);
})();
