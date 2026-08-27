/*
 * Atlantic Browser — the browser.* / chrome.* shim injected into every
 * extension context (content-script isolated worlds and the JSC background
 * host alike).
 *
 * One async bridge carries everything. JS calls __atlExtPost(json) with
 * {seq, api, args}; the UI process answers by calling __atlExtBridge.dispatch()
 * with either {type:"reply", seq, ok, value} or {type:"event", name, args}.
 * Everything above that — promises vs. callbacks, Events, ports, storage areas,
 * i18n — is implemented here so the C++ side only ever sees flat API calls.
 *
 * Tokens substituted by WebExtensionManager before injection:
 *   @@ATL_EXT_ID@@    the extension id, JSON-quoted
 *   @@ATL_MANIFEST@@  the full manifest object
 *   @@ATL_L10N@@      flattened messages.json for the active locale
 *   @@ATL_CONTEXT@@   "content" or "background", JSON-quoted
 *   @@ATL_HANDLER@@   script-message-handler name, JSON-quoted (content only)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

namespace WebExtensionScripts {

static const char* const kApiShim = R"JS(
(function(global) {
    if (global.__atlExtBridge) return;

    var EXT_ID = @@ATL_EXT_ID@@;
    var MANIFEST = @@ATL_MANIFEST@@;
    var L10N = @@ATL_L10N@@;
    var CONTEXT = @@ATL_CONTEXT@@;
    var HANDLER = @@ATL_HANDLER@@;
    var BASE_URL = "atlantic-extension://" + EXT_ID + "/";

    // --- transport ----------------------------------------------------------

    function post(payload) {
        var json = JSON.stringify(payload);
        if (CONTEXT === "background") {
            global.__atlNative(json);
        } else {
            var handlers = global.webkit && global.webkit.messageHandlers;
            if (handlers && handlers[HANDLER])
                handlers[HANDLER].postMessage(json);
        }
    }

    var nextSeq = 1;
    var pending = Object.create(null);

    function call(api, args) {
        return new Promise(function(resolve, reject) {
            var seq = nextSeq++;
            pending[seq] = { resolve: resolve, reject: reject };
            post({ seq: seq, api: api, args: args || [] });
        });
    }

    function notify(api, args) {
        post({ seq: 0, api: api, args: args || [] });
    }

    // --- promise/callback duality -------------------------------------------
    //
    // MV2 Chrome extensions pass a callback and read chrome.runtime.lastError;
    // Firefox/MV3 code awaits a promise. Accept both: a trailing function
    // argument switches to callback style.

    var lastError = null;

    function wrap(api, fixedArgs) {
        return function() {
            var args = Array.prototype.slice.call(arguments);
            var callback = null;
            if (args.length && typeof args[args.length - 1] === "function")
                callback = args.pop();
            if (fixedArgs)
                args = fixedArgs.concat(args);
            var promise = call(api, args);
            if (!callback)
                return promise;
            promise.then(function(value) {
                lastError = null;
                try { callback(value); } catch (e) { reportError(e); }
            }, function(error) {
                lastError = { message: String(error && error.message || error) };
                try { callback(undefined); } catch (e) { reportError(e); }
                // Chrome logs an unchecked lastError; we surface it either way.
                lastError = null;
            });
            return undefined;
        };
    }

    function reportError(e) {
        notify("log", ["error", String(e && e.stack || e)]);
    }

    // --- Event --------------------------------------------------------------

    function Event(name) {
        this._name = name;
        this._listeners = [];
    }
    Event.prototype.addListener = function(fn) {
        if (typeof fn === "function" && this._listeners.indexOf(fn) === -1)
            this._listeners.push(fn);
    };
    Event.prototype.removeListener = function(fn) {
        var i = this._listeners.indexOf(fn);
        if (i !== -1) this._listeners.splice(i, 1);
    };
    Event.prototype.hasListener = function(fn) {
        return this._listeners.indexOf(fn) !== -1;
    };
    Event.prototype.hasListeners = function() {
        return this._listeners.length > 0;
    };
    Event.prototype._emit = function(args) {
        var results = [];
        var snapshot = this._listeners.slice();
        for (var i = 0; i < snapshot.length; i++) {
            try {
                results.push(snapshot[i].apply(null, args));
            } catch (e) {
                reportError(e);
            }
        }
        return results;
    };

    // An API we do not implement. Calling it rejects with a clear message, but
    // addListener on its events stays silent: extensions routinely register
    // webRequest/contextMenus listeners at top level, and throwing there would
    // take down the whole background script instead of degrading one feature.
    var warned = Object.create(null);
    function unsupported(apiName) {
        return function() {
            if (!warned[apiName]) {
                warned[apiName] = true;
                notify("log", ["warn", "browser." + apiName + " is not supported by Atlantic"]);
            }
            var args = Array.prototype.slice.call(arguments);
            var callback = (args.length && typeof args[args.length - 1] === "function") ? args.pop() : null;
            var error = new Error(apiName + " is not supported by Atlantic");
            if (callback) {
                lastError = { message: error.message };
                try { callback(undefined); } catch (e) { reportError(e); }
                lastError = null;
                return undefined;
            }
            return Promise.reject(error);
        };
    }
    function inertEvent(name) {
        var event = new Event(name);
        // Never emitted; keep addListener/removeListener working.
        return event;
    }

    // --- ports --------------------------------------------------------------

    var ports = Object.create(null);
    var nextPortId = 1;

    function makePort(portId, name, sender) {
        var port = {
            name: name || "",
            sender: sender || undefined,
            onMessage: new Event("port.onMessage"),
            onDisconnect: new Event("port.onDisconnect"),
            postMessage: function(message) {
                notify("runtime.portMessage", [portId, message]);
            },
            disconnect: function() {
                delete ports[portId];
                notify("runtime.portDisconnect", [portId]);
            }
        };
        ports[portId] = port;
        return port;
    }

    // --- storage ------------------------------------------------------------

    function storageArea(area) {
        return {
            get: function(keys, callback) {
                if (typeof keys === "function") { callback = keys; keys = null; }
                var normalized = keys === undefined ? null : keys;
                return callback
                    ? wrap("storage.get", [area])(normalized, callback)
                    : wrap("storage.get", [area])(normalized);
            },
            set: wrap("storage.set", [area]),
            remove: wrap("storage.remove", [area]),
            clear: wrap("storage.clear", [area]),
            getBytesInUse: wrap("storage.getBytesInUse", [area])
        };
    }
    var storage = {
        local: storageArea("local"),
        sync: storageArea("sync"),
        session: storageArea("session"),
        managed: storageArea("managed"),
        onChanged: new Event("storage.onChanged")
    };

    // --- i18n ---------------------------------------------------------------

    function getMessage(key, substitutions) {
        var entry = L10N[String(key).toLowerCase()];
        if (!entry) {
            // Predefined messages that never appear in messages.json.
            if (key === "@@extension_id") return EXT_ID;
            if (key === "@@ui_locale") return (global.navigator && navigator.language || "en").replace("-", "_");
            if (key === "@@bidi_dir") return "ltr";
            if (key === "@@bidi_reversed_dir") return "rtl";
            if (key === "@@bidi_start_edge") return "left";
            if (key === "@@bidi_end_edge") return "right";
            return "";
        }
        var text = entry.message || "";
        var subs = substitutions === undefined ? []
            : (Array.isArray(substitutions) ? substitutions : [substitutions]);

        // Named placeholders resolve first ($NAME$ -> its "content", which is
        // itself a $1..$9 reference into `subs`).
        if (entry.placeholders) {
            for (var name in entry.placeholders) {
                var content = entry.placeholders[name].content || "";
                text = text.replace(new RegExp("\\$" + name + "\\$", "gi"), content);
            }
        }
        text = text.replace(/\$(\d)/g, function(match, index) {
            var value = subs[Number(index) - 1];
            return value === undefined ? "" : String(value);
        });
        return text.replace(/\$\$/g, "$");
    }

    // --- API surface --------------------------------------------------------

    var runtime = {
        id: EXT_ID,
        getURL: function(path) {
            return BASE_URL + String(path || "").replace(/^\/+/, "");
        },
        getManifest: function() {
            return JSON.parse(JSON.stringify(MANIFEST));
        },
        getPlatformInfo: wrap("runtime.getPlatformInfo"),
        openOptionsPage: wrap("runtime.openOptionsPage"),
        reload: wrap("runtime.reload"),
        sendMessage: function() {
            // (extensionId?, message, options?, callback?)
            var args = Array.prototype.slice.call(arguments);
            var callback = (args.length && typeof args[args.length - 1] === "function") ? args.pop() : null;
            var extensionId = null;
            var message;
            if (args.length > 1 && (typeof args[0] === "string" || args[0] === null)) {
                extensionId = args[0];
                message = args[1];
            } else {
                message = args[0];
            }
            var promise = call("runtime.sendMessage", [extensionId, message]);
            if (!callback) return promise;
            promise.then(function(v) { callback(v); }, function(e) {
                lastError = { message: String(e && e.message || e) };
                try { callback(undefined); } catch (err) { reportError(err); }
                lastError = null;
            });
            return undefined;
        },
        connect: function(extensionId, connectInfo) {
            if (typeof extensionId === "object" && extensionId !== null) {
                connectInfo = extensionId;
                extensionId = null;
            }
            var portId = "p" + (CONTEXT === "background" ? "b" : "c") + (nextPortId++);
            var name = (connectInfo && connectInfo.name) || "";
            var port = makePort(portId, name, null);
            notify("runtime.connect", [portId, name, null]);
            return port;
        },
        onMessage: new Event("runtime.onMessage"),
        onMessageExternal: inertEvent("runtime.onMessageExternal"),
        onConnect: new Event("runtime.onConnect"),
        onConnectExternal: inertEvent("runtime.onConnectExternal"),
        onInstalled: new Event("runtime.onInstalled"),
        onStartup: new Event("runtime.onStartup"),
        onSuspend: inertEvent("runtime.onSuspend"),
        onUpdateAvailable: inertEvent("runtime.onUpdateAvailable"),
        // Called unconditionally by extensions that set an uninstall survey
        // URL. Atlantic has nowhere to send it, but throwing here would take
        // down the rest of a background script, so it is accepted and dropped.
        setUninstallURL: function(url, callback) {
            if (callback) { callback(); return undefined; }
            return Promise.resolve();
        },
        getFrameId: function() { return -1; },
        connectNative: unsupported("runtime.connectNative"),
        sendNativeMessage: unsupported("runtime.sendNativeMessage"),
        getPackageDirectoryEntry: unsupported("runtime.getPackageDirectoryEntry"),
        getBackgroundPage: function(callback) {
            // We have no DOM window for the background context, so there is
            // nothing meaningful to hand back.
            if (callback) { callback(null); return undefined; }
            return Promise.resolve(null);
        }
    };
    Object.defineProperty(runtime, "lastError", {
        get: function() { return lastError; },
        enumerable: true
    });

    var tabs = {
        query: wrap("tabs.query"),
        get: wrap("tabs.get"),
        getCurrent: wrap("tabs.getCurrent"),
        create: wrap("tabs.create"),
        update: wrap("tabs.update"),
        remove: wrap("tabs.remove"),
        reload: wrap("tabs.reload"),
        sendMessage: function(tabId, message, options, callback) {
            if (typeof options === "function") { callback = options; options = null; }
            var promise = call("tabs.sendMessage", [tabId, message]);
            if (!callback) return promise;
            promise.then(function(v) { callback(v); }, function(e) {
                lastError = { message: String(e && e.message || e) };
                try { callback(undefined); } catch (err) { reportError(err); }
                lastError = null;
            });
            return undefined;
        },
        connect: function(tabId, connectInfo) {
            var portId = "pt" + (nextPortId++);
            var name = (connectInfo && connectInfo.name) || "";
            var port = makePort(portId, name, null);
            notify("runtime.connect", [portId, name, tabId]);
            return port;
        },
        executeScript: wrap("tabs.executeScript"),
        insertCSS: wrap("tabs.insertCSS"),
        removeCSS: wrap("tabs.removeCSS"),
        onCreated: new Event("tabs.onCreated"),
        onUpdated: new Event("tabs.onUpdated"),
        onRemoved: new Event("tabs.onRemoved"),
        onActivated: new Event("tabs.onActivated"),
        onReplaced: inertEvent("tabs.onReplaced")
    };

    var action = {
        setBadgeText: wrap("action.setBadgeText"),
        getBadgeText: wrap("action.getBadgeText"),
        setBadgeBackgroundColor: wrap("action.setBadgeBackgroundColor"),
        getBadgeBackgroundColor: wrap("action.getBadgeBackgroundColor"),
        setTitle: wrap("action.setTitle"),
        getTitle: wrap("action.getTitle"),
        setIcon: wrap("action.setIcon"),
        setPopup: wrap("action.setPopup"),
        getPopup: wrap("action.getPopup"),
        openPopup: wrap("action.openPopup"),
        enable: wrap("action.enable"),
        disable: wrap("action.disable"),
        onClicked: new Event("action.onClicked")
    };

    var permissions = {
        getAll: wrap("permissions.getAll"),
        contains: wrap("permissions.contains"),
        // Everything an extension can get is granted at install time; there is
        // no runtime prompt, so a request for something undeclared is denied.
        request: wrap("permissions.request"),
        remove: wrap("permissions.remove"),
        onAdded: inertEvent("permissions.onAdded"),
        onRemoved: inertEvent("permissions.onRemoved")
    };

    var windows = {
        WINDOW_ID_NONE: -1,
        WINDOW_ID_CURRENT: -2,
        getCurrent: wrap("windows.getCurrent"),
        getLastFocused: wrap("windows.getCurrent"),
        getAll: wrap("windows.getAll"),
        get: wrap("windows.getCurrent"),
        create: wrap("windows.create"),
        update: wrap("windows.update"),
        remove: unsupported("windows.remove"),
        onCreated: inertEvent("windows.onCreated"),
        onRemoved: inertEvent("windows.onRemoved"),
        onFocusChanged: inertEvent("windows.onFocusChanged")
    };

    var notifications = {
        create: wrap("notifications.create"),
        clear: wrap("notifications.clear"),
        getAll: wrap("notifications.getAll"),
        update: unsupported("notifications.update"),
        onClicked: new Event("notifications.onClicked"),
        onClosed: new Event("notifications.onClosed"),
        onButtonClicked: inertEvent("notifications.onButtonClicked")
    };

    // alarms is implemented locally on top of timers rather than round-tripping
    // to C++: the background host already provides setTimeout/setInterval.
    var alarmTable = Object.create(null);
    var alarms = {
        onAlarm: new Event("alarms.onAlarm"),
        create: function(name, info) {
            if (typeof name === "object") { info = name; name = ""; }
            name = name || "";
            info = info || {};
            alarms.clear(name);
            var when = info.when ? (info.when - Date.now())
                : (info.delayInMinutes ? info.delayInMinutes * 60000 : 0);
            var period = info.periodInMinutes ? info.periodInMinutes * 60000 : 0;
            var fire = function() {
                alarms.onAlarm._emit([{ name: name, scheduledTime: Date.now(),
                                        periodInMinutes: info.periodInMinutes }]);
            };
            var entry = {};
            entry.timeout = global.setTimeout(function() {
                fire();
                if (period)
                    entry.interval = global.setInterval(fire, period);
                else
                    delete alarmTable[name];
            }, Math.max(0, when));
            alarmTable[name] = entry;
        },
        clear: function(name, callback) {
            name = name || "";
            var entry = alarmTable[name];
            if (entry) {
                if (entry.timeout) global.clearTimeout(entry.timeout);
                if (entry.interval) global.clearInterval(entry.interval);
                delete alarmTable[name];
            }
            if (callback) { callback(!!entry); return undefined; }
            return Promise.resolve(!!entry);
        },
        clearAll: function(callback) {
            for (var name in alarmTable) alarms.clear(name);
            if (callback) { callback(true); return undefined; }
            return Promise.resolve(true);
        },
        get: function(name, callback) {
            var result = alarmTable[name || ""] ? { name: name || "" } : undefined;
            if (callback) { callback(result); return undefined; }
            return Promise.resolve(result);
        },
        getAll: function(callback) {
            var all = Object.keys(alarmTable).map(function(n) { return { name: n }; });
            if (callback) { callback(all); return undefined; }
            return Promise.resolve(all);
        }
    };

    // contextMenus.create is the odd one out: it returns the item id
    // *synchronously* and only reports failure through the optional callback,
    // so an id has to be minted here when the caller did not supply one.
    var nextMenuId = 1;
    var contextMenus = {
        create: function(properties, callback) {
            properties = properties || {};
            if (properties.id === undefined || properties.id === null)
                properties.id = "atl-item-" + (nextMenuId++);
            var promise = call("contextMenus.create", [properties]);
            if (callback)
                promise.then(function() { callback(); }, function() { callback(); });
            else
                promise.catch(function(e) { reportError(e); });
            return properties.id;
        },
        update: wrap("contextMenus.update"),
        remove: wrap("contextMenus.remove"),
        removeAll: wrap("contextMenus.removeAll"),
        onClicked: new Event("contextMenus.onClicked"),
        onShown: inertEvent("contextMenus.onShown"),
        onHidden: inertEvent("contextMenus.onHidden"),
        ACTION_MENU_TOP_LEVEL_LIMIT: 6
    };

    function unsupportedNamespace(name, methods, events) {
        var namespace = {};
        methods.forEach(function(method) { namespace[method] = unsupported(name + "." + method); });
        (events || []).forEach(function(event) { namespace[event] = inertEvent(name + "." + event); });
        return namespace;
    }

    var api = {
        runtime: runtime,
        storage: storage,
        tabs: tabs,
        action: action,
        browserAction: action,
        pageAction: action,
        permissions: permissions,
        windows: windows,
        notifications: notifications,
        alarms: alarms,
        i18n: {
            getMessage: getMessage,
            getUILanguage: function() {
                return (global.navigator && navigator.language) || "en";
            },
            getAcceptLanguages: function(callback) {
                var value = [(global.navigator && navigator.language) || "en"];
                if (callback) { callback(value); return undefined; }
                return Promise.resolve(value);
            },
            detectLanguage: unsupported("i18n.detectLanguage")
        },
        extension: {
            getURL: runtime.getURL,
            getBackgroundPage: runtime.getBackgroundPage,
            inIncognitoContext: false,
            isAllowedIncognitoAccess: function(callback) {
                if (callback) { callback(false); return undefined; }
                return Promise.resolve(false);
            },
            getViews: function() { return []; }
        },
        // Deliberately inert. Network blocking lives in the Rust adblock
        // WebProcess extension, which extensions cannot reach; see
        // docs/investigations/webextensions.md.
        webRequest: unsupportedNamespace("webRequest",
            ["handlerBehaviorChanged"],
            ["onBeforeRequest", "onBeforeSendHeaders", "onSendHeaders",
             "onHeadersReceived", "onBeforeRedirect", "onResponseStarted",
             "onCompleted", "onErrorOccurred", "onAuthRequired"]),
        declarativeNetRequest: unsupportedNamespace("declarativeNetRequest",
            ["updateDynamicRules", "getDynamicRules", "updateEnabledRulesets",
             "getEnabledRulesets", "updateSessionRules", "getSessionRules"],
            ["onRuleMatchedDebug"]),
        // Main frame only: Atlantic reports no per-subframe navigation, so the
        // frame-scoped calls answer for frame 0 and the subframe-only events
        // stay inert rather than lying about ids.
        webNavigation: {
            getFrame: function(details, callback) {
                var value = { frameId: 0, parentFrameId: -1, errorOccurred: false };
                if (callback) { callback(value); return undefined; }
                return Promise.resolve(value);
            },
            getAllFrames: function(details, callback) {
                var value = [{ frameId: 0, parentFrameId: -1, errorOccurred: false }];
                if (callback) { callback(value); return undefined; }
                return Promise.resolve(value);
            },
            onBeforeNavigate: new Event("webNavigation.onBeforeNavigate"),
            onCommitted: new Event("webNavigation.onCommitted"),
            onCompleted: new Event("webNavigation.onCompleted"),
            onDOMContentLoaded: new Event("webNavigation.onDOMContentLoaded"),
            onHistoryStateUpdated: new Event("webNavigation.onHistoryStateUpdated"),
            onErrorOccurred: new Event("webNavigation.onErrorOccurred"),
            onCreatedNavigationTarget: inertEvent("webNavigation.onCreatedNavigationTarget"),
            onReferenceFragmentUpdated: inertEvent("webNavigation.onReferenceFragmentUpdated"),
            onTabReplaced: inertEvent("webNavigation.onTabReplaced")
        },
        // Backed by the default network session's cookie jar. There is one
        // store, "0"; private-browsing cookies live on an ephemeral session
        // and are deliberately not reachable. onChanged fires for changes made
        // through this API - WebKit reports nothing when a page sets a cookie.
        cookies: {
            get: wrap("cookies.get"),
            getAll: wrap("cookies.getAll"),
            set: wrap("cookies.set"),
            remove: wrap("cookies.remove"),
            getAllCookieStores: wrap("cookies.getAllCookieStores"),
            onChanged: new Event("cookies.onChanged"),
            SameSiteStatus: {
                NO_RESTRICTION: "no_restriction",
                LAX: "lax",
                STRICT: "strict",
                UNSPECIFIED: "unspecified"
            },
            OnChangedCause: {
                EVICTED: "evicted",
                EXPIRED: "expired",
                EXPLICIT: "explicit",
                EXPIRED_OVERWRITE: "expired_overwrite",
                OVERWRITE: "overwrite"
            }
        },
        contextMenus: contextMenus,
        menus: contextMenus,
        scripting: {
            // `func` is a live function; it cannot be serialised as-is, so it
            // travels as source text and is re-created on the page side. That
            // is also why it must not close over anything - same rule the real
            // API has, for the same reason.
            executeScript: function(injection, callback) {
                var payload = {};
                for (var key in injection) {
                    if (key !== "func")
                        payload[key] = injection[key];
                }
                if (typeof injection.func === "function")
                    payload.funcSource = injection.func.toString();
                return callback ? wrap("scripting.executeScript")(payload, callback)
                                : wrap("scripting.executeScript")(payload);
            },
            insertCSS: wrap("scripting.insertCSS"),
            removeCSS: wrap("scripting.removeCSS"),
            registerContentScripts: wrap("scripting.registerContentScripts"),
            unregisterContentScripts: wrap("scripting.unregisterContentScripts"),
            updateContentScripts: wrap("scripting.registerContentScripts"),
            getRegisteredContentScripts: wrap("scripting.getRegisteredContentScripts"),
            ExecutionWorld: { ISOLATED: "ISOLATED", MAIN: "MAIN" }
        },
        // DownloadManager drives the transfer; its per-download record is what
        // search() reads, and it lives for this run of the browser only.
        // pause/resume have no equivalent in WebKit's download API, and there
        // is nothing to hand a finished file to, so open/show stay unsupported.
        downloads: {
            download: wrap("downloads.download"),
            search: wrap("downloads.search"),
            cancel: wrap("downloads.cancel"),
            erase: wrap("downloads.erase"),
            removeFile: wrap("downloads.removeFile"),
            pause: unsupported("downloads.pause"),
            resume: unsupported("downloads.resume"),
            open: unsupported("downloads.open"),
            show: unsupported("downloads.show"),
            showDefaultFolder: unsupported("downloads.showDefaultFolder"),
            getFileIcon: unsupported("downloads.getFileIcon"),
            acceptDanger: unsupported("downloads.acceptDanger"),
            onCreated: new Event("downloads.onCreated"),
            onChanged: new Event("downloads.onChanged"),
            onErased: new Event("downloads.onErased"),
            State: { IN_PROGRESS: "in_progress", INTERRUPTED: "interrupted", COMPLETE: "complete" }
        },
        // The browser's own history database. One row per URL, so getVisits
        // reports the last visit rather than inventing the earlier ones, and
        // onVisited fires when a load finishes in a non-private tab.
        history: {
            search: wrap("history.search"),
            getVisits: wrap("history.getVisits"),
            addUrl: wrap("history.addUrl"),
            deleteUrl: wrap("history.deleteUrl"),
            deleteRange: wrap("history.deleteRange"),
            deleteAll: wrap("history.deleteAll"),
            onVisited: new Event("history.onVisited"),
            onVisitRemoved: new Event("history.onVisitRemoved"),
            TransitionType: { LINK: "link", TYPED: "typed" }
        },
        // The browser's bookmark list, which is flat: everything lives in one
        // folder under the root, and create() will not make folders.
        bookmarks: {
            get: wrap("bookmarks.get"),
            getChildren: wrap("bookmarks.getChildren"),
            getTree: wrap("bookmarks.getTree"),
            getSubTree: wrap("bookmarks.getSubTree"),
            getRecent: wrap("bookmarks.getRecent"),
            search: wrap("bookmarks.search"),
            create: wrap("bookmarks.create"),
            remove: wrap("bookmarks.remove"),
            removeTree: wrap("bookmarks.removeTree"),
            update: wrap("bookmarks.update"),
            // No reordering in a flat list with no drag surface for it.
            move: unsupported("bookmarks.move"),
            onCreated: new Event("bookmarks.onCreated"),
            onRemoved: new Event("bookmarks.onRemoved"),
            onChanged: new Event("bookmarks.onChanged"),
            onMoved: inertEvent("bookmarks.onMoved"),
            BookmarkTreeNodeType: { BOOKMARK: "bookmark", FOLDER: "folder", SEPARATOR: "separator" }
        },
        management: {
            getSelf: wrap("management.getSelf"),
            // Enumerating or disabling other extensions is deliberately not
            // offered; an extension may only ask about itself.
            get: unsupported("management.get"),
            getAll: unsupported("management.getAll"),
            setEnabled: unsupported("management.setEnabled"),
            uninstallSelf: unsupported("management.uninstallSelf"),
            onInstalled: inertEvent("management.onInstalled"),
            onUninstalled: inertEvent("management.onUninstalled")
        },
        idle: unsupportedNamespace("idle", ["queryState", "setDetectionInterval"], ["onStateChanged"]),
        privacy: { network: {}, services: {}, websites: {} },
        proxy: unsupportedNamespace("proxy", ["register", "unregister"], ["onRequest", "onError"])
    };

    // --- inbound dispatch ---------------------------------------------------

    var bridge = {};

    bridge.dispatch = function(json) {
        var payload;
        try {
            payload = typeof json === "string" ? JSON.parse(json) : json;
        } catch (e) {
            reportError(e);
            return;
        }
        if (payload.type === "reply") {
            var entry = pending[payload.seq];
            if (!entry) return;
            delete pending[payload.seq];
            if (payload.ok)
                entry.resolve(payload.value);
            else
                entry.reject(new Error(payload.error || "extension API call failed"));
            return;
        }
        if (payload.type === "event")
            handleEvent(payload.name, payload.args || []);
    };

    function handleEvent(name, args) {
        switch (name) {
        case "runtime.onMessage":
            deliverMessage(runtime.onMessage, args);
            return;
        case "runtime.onConnect": {
            // args: [portId, name, sender]
            var port = makePort(args[0], args[1], args[2]);
            runtime.onConnect._emit([port]);
            return;
        }
        case "runtime.portMessage": {
            var target = ports[args[0]];
            if (target) target.onMessage._emit([args[1], target]);
            return;
        }
        case "runtime.portDisconnect": {
            var closing = ports[args[0]];
            if (closing) {
                delete ports[args[0]];
                closing.onDisconnect._emit([closing]);
            }
            return;
        }
        case "storage.onChanged":
            storage.onChanged._emit([args[0], args[1]]);
            return;
        }

        // Everything else is a plain fan-out to the matching Event object.
        var parts = name.split(".");
        var namespace = api[parts[0]];
        var event = namespace && namespace[parts[1]];
        if (event && typeof event._emit === "function")
            event._emit(args);
    }

    // onMessage has the awkward contract: a listener may answer synchronously
    // by returning a value, asynchronously by returning true and calling
    // sendResponse later, or by returning a promise (Firefox style). We must
    // tell the UI process whether anyone is going to answer at all, so it can
    // resolve the sender's promise instead of hanging.
    function deliverMessage(event, args) {
        var message = args[0];
        var sender = args[1];
        var token = args[2];
        var answered = false;

        function sendResponse(response) {
            if (answered) return;
            answered = true;
            notify("runtime.onMessageResponse", [token, true, response === undefined ? null : response]);
        }

        var listeners = event._listeners.slice();
        var wantsAsync = false;
        for (var i = 0; i < listeners.length; i++) {
            var result;
            try {
                result = listeners[i](message, sender, sendResponse);
            } catch (e) {
                reportError(e);
                continue;
            }
            if (result === true) {
                wantsAsync = true;
            } else if (result && typeof result.then === "function") {
                wantsAsync = true;
                result.then(sendResponse, function(e) {
                    reportError(e);
                    sendResponse(undefined);
                });
            } else if (result !== undefined) {
                sendResponse(result);
                return;
            }
        }
        if (!answered && !wantsAsync)
            notify("runtime.onMessageResponse", [token, false, null]);
    }

    bridge.hasMessageListeners = function() {
        return runtime.onMessage.hasListeners();
    };

    global.__atlExtBridge = bridge;
    global.browser = api;
    global.chrome = api;
})(typeof globalThis !== "undefined" ? globalThis : this);
)JS";

// Runs in the JSC background context only, before the shim. JSC gives us a bare
// ECMAScript global: no timers, no console, no network. Each of these is a thin
// wrapper over the same bridge, so the C++ host implements them once.
static const char* const kBackgroundPreamble = R"JS(
(function(global) {
    if (global.__atlBackgroundReady) return;
    global.__atlBackgroundReady = true;

    var nextTimer = 1;
    var timers = Object.create(null);

    global.setTimeout = function(fn, delay) {
        var extra = Array.prototype.slice.call(arguments, 2);
        var id = nextTimer++;
        timers[id] = { fn: fn, args: extra, repeat: false };
        global.__atlNative(JSON.stringify({ seq: 0, api: "timer.set",
                                           args: [id, Number(delay) || 0, false] }));
        return id;
    };
    global.setInterval = function(fn, delay) {
        var extra = Array.prototype.slice.call(arguments, 2);
        var id = nextTimer++;
        timers[id] = { fn: fn, args: extra, repeat: true };
        global.__atlNative(JSON.stringify({ seq: 0, api: "timer.set",
                                           args: [id, Number(delay) || 0, true] }));
        return id;
    };
    global.clearTimeout = global.clearInterval = function(id) {
        if (!timers[id]) return;
        delete timers[id];
        global.__atlNative(JSON.stringify({ seq: 0, api: "timer.clear", args: [id] }));
    };
    // Queue a microtask-ish callback; JSC has Promise so this is enough.
    global.queueMicrotask = global.queueMicrotask || function(fn) {
        Promise.resolve().then(fn);
    };

    global.__atlFireTimer = function(id) {
        var entry = timers[id];
        if (!entry) return;
        if (!entry.repeat)
            delete timers[id];
        try {
            entry.fn.apply(null, entry.args);
        } catch (e) {
            global.__atlNative(JSON.stringify({ seq: 0, api: "log",
                                                args: ["error", String(e && e.stack || e)] }));
        }
    };

    // A minimal EventTarget on the global. MV3 background scripts are written
    // as service workers and routinely call self.addEventListener("error"|
    // "unhandledrejection"|"install"|...) at top level; without this the very
    // first such call throws and takes the rest of the script with it.
    var globalListeners = Object.create(null);
    global.addEventListener = function(type, fn) {
        if (typeof fn !== "function") return;
        var list = globalListeners[type] || (globalListeners[type] = []);
        if (list.indexOf(fn) === -1) list.push(fn);
    };
    global.removeEventListener = function(type, fn) {
        var list = globalListeners[type];
        if (!list) return;
        var i = list.indexOf(fn);
        if (i !== -1) list.splice(i, 1);
    };
    global.dispatchEvent = function(event) {
        if (!event || !event.type) return true;
        var list = (globalListeners[event.type] || []).slice();
        for (var i = 0; i < list.length; i++) {
            try {
                list[i].call(global, event);
            } catch (e) {
                global.__atlNative(JSON.stringify({ seq: 0, api: "log",
                    args: ["error", String(e && e.stack || e)] }));
            }
        }
        return !event.defaultPrevented;
    };

    // Reported errors reach any "error" listener, so an extension's own
    // telemetry sees what we log.
    global.__atlDispatchError = function(message, stack) {
        global.dispatchEvent({ type: "error", message: message, filename: "",
                               lineno: 0, colno: 0,
                               error: { message: message, stack: stack || "" } });
    };

    // Service-worker lifecycle calls that scripts make unconditionally. There
    // is no worker here, so these are no-ops rather than errors.
    global.skipWaiting = function() { return Promise.resolve(); };
    global.registration = undefined;

    var console = {};
    ["log", "info", "warn", "error", "debug", "trace"].forEach(function(level) {
        console[level] = function() {
            var parts = [];
            for (var i = 0; i < arguments.length; i++) {
                var value = arguments[i];
                try {
                    parts.push(typeof value === "string" ? value : JSON.stringify(value));
                } catch (e) {
                    parts.push(String(value));
                }
            }
            global.__atlNative(JSON.stringify({ seq: 0, api: "log",
                                                args: [level === "debug" || level === "trace" ? "log" : level,
                                                       parts.join(" ")] }));
        };
    });
    console.dir = console.log;
    console.group = console.groupEnd = console.time = console.timeEnd = function() {};
    global.console = console;

    // Minimal navigator/location so feature-detecting extension code does not
    // trip over a bare global.
    global.navigator = global.navigator || {
        userAgent: "Atlantic", language: "en", languages: ["en"], onLine: true,
        platform: "Linux aarch64"
    };
    global.self = global;
    global.window = undefined;

    global.atob = global.atob || function(input) {
        var chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        var str = String(input).replace(/=+$/, "");
        var output = "";
        for (var bc = 0, bs = 0, buffer, i = 0; (buffer = str.charAt(i++));) {
            buffer = chars.indexOf(buffer);
            if (~buffer) {
                bs = bc % 4 ? bs * 64 + buffer : buffer;
                if (bc++ % 4)
                    output += String.fromCharCode(255 & (bs >> ((-2 * bc) & 6)));
            }
        }
        return output;
    };
    global.btoa = global.btoa || function(input) {
        var chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        var str = String(input);
        var output = "";
        for (var block = 0, charCode, i = 0, map = chars;
             str.charAt(i | 0) || ((map = "="), i % 1);
             output += map.charAt(63 & (block >> (8 - (i % 1) * 8)))) {
            charCode = str.charCodeAt((i += 3 / 4));
            block = (block << 8) | charCode;
        }
        return output;
    };

    // --- Web platform globals JavaScriptCore does not have --------------------
    //
    // URL, Headers, AbortController and friends live in WebCore, not in the
    // engine, so a bare jsc_context_new() global has none of them. Background
    // scripts written as service workers use them constantly - LanguageTool's
    // reaches for URL 63 times and AbortController 46 - and the failures land
    // inside promise handlers at request time, which is why the script can run
    // to completion and still never do any work.

    function ltrim(s, ch) { while (s.charAt(0) === ch) s = s.substring(1); return s; }

    global.URLSearchParams = function(init) {
        var pairs = [];
        if (typeof init === "string") {
            ltrim(init, "?").split("&").forEach(function(part) {
                if (!part) return;
                var i = part.indexOf("=");
                var k = i < 0 ? part : part.substring(0, i);
                var v = i < 0 ? "" : part.substring(i + 1);
                pairs.push([decodeURIComponent(k.replace(/\+/g, " ")),
                            decodeURIComponent(v.replace(/\+/g, " "))]);
            });
        } else if (init && typeof init === "object") {
            if (Array.isArray(init))
                init.forEach(function(p) { pairs.push([String(p[0]), String(p[1])]); });
            else
                for (var k in init) pairs.push([k, String(init[k])]);
        }
        this._pairs = pairs;
    };
    global.URLSearchParams.prototype = {
        append: function(k, v) { this._pairs.push([String(k), String(v)]); },
        set: function(k, v) {
            var found = false;
            this._pairs = this._pairs.filter(function(p) {
                if (p[0] !== String(k)) return true;
                if (found) return false;
                found = true; p[1] = String(v); return true;
            });
            if (!found) this._pairs.push([String(k), String(v)]);
        },
        get: function(k) {
            for (var i = 0; i < this._pairs.length; i++)
                if (this._pairs[i][0] === String(k)) return this._pairs[i][1];
            return null;
        },
        getAll: function(k) {
            return this._pairs.filter(function(p) { return p[0] === String(k); })
                              .map(function(p) { return p[1]; });
        },
        has: function(k) { return this.get(k) !== null; },
        "delete": function(k) {
            this._pairs = this._pairs.filter(function(p) { return p[0] !== String(k); });
        },
        forEach: function(fn, self_) {
            this._pairs.forEach(function(p) { fn.call(self_, p[1], p[0], this); }, this);
        },
        keys: function() { return this._pairs.map(function(p) { return p[0]; }); },
        values: function() { return this._pairs.map(function(p) { return p[1]; }); },
        toString: function() {
            return this._pairs.map(function(p) {
                return encodeURIComponent(p[0]) + "=" + encodeURIComponent(p[1]);
            }).join("&");
        }
    };

    // Enough of the URL parser for absolute URLs and relative resolution, which
    // is all an extension does with it. Not the full WHATWG algorithm: no IDNA,
    // no percent-encoding normalisation of the host.
    var URL_RE = /^([a-zA-Z][a-zA-Z0-9+.-]*:)\/\/([^\/?#]*)([^?#]*)(\?[^#]*)?(#.*)?$/;

    function resolvePath(basePath, relative) {
        if (relative.charAt(0) === "/") return relative;
        // Join onto the base's directory, then fold "." and ".." away. The
        // leading empty segment is the root and must survive, hence length > 1.
        var directory = basePath.replace(/[^\/]*$/, "");
        var segments = (directory + relative).split("/");
        var out = [];
        for (var i = 0; i < segments.length; i++) {
            var part = segments[i];
            if (part === ".") continue;
            if (part === "..") { if (out.length > 1) out.pop(); continue; }
            out.push(part);
        }
        return out.join("/") || "/";
    }

    global.URL = function(input, base) {
        var href = String(input);
        if (!URL_RE.test(href)) {
            if (base === undefined)
                throw new TypeError("Invalid URL: " + href);
            var b = new global.URL(base);
            if (href.indexOf("//") === 0)
                href = b.protocol + href;
            else if (href.charAt(0) === "?")
                href = b.protocol + "//" + b.host + b.pathname + href;
            else if (href.charAt(0) === "#")
                href = b.protocol + "//" + b.host + b.pathname + b.search + href;
            else
                href = b.protocol + "//" + b.host + resolvePath(b.pathname, href);
        }

        var m = URL_RE.exec(href);
        if (!m) throw new TypeError("Invalid URL: " + href);
        this.protocol = m[1];
        var authority = m[2];
        var at = authority.lastIndexOf("@");
        if (at !== -1) {
            var credentials = authority.substring(0, at).split(":");
            this.username = credentials[0] || "";
            this.password = credentials[1] || "";
            authority = authority.substring(at + 1);
        } else {
            this.username = "";
            this.password = "";
        }
        var colon = authority.lastIndexOf(":");
        if (colon !== -1 && authority.indexOf("]") < colon) {
            this.hostname = authority.substring(0, colon);
            this.port = authority.substring(colon + 1);
        } else {
            this.hostname = authority;
            this.port = "";
        }
        this.host = authority;
        this.pathname = m[3] || "/";
        this.search = m[4] || "";
        this.hash = m[5] || "";
        this.origin = this.protocol + "//" + this.host;
        this.searchParams = new global.URLSearchParams(this.search);
        this.href = this.protocol + "//"
            + (this.username ? this.username + (this.password ? ":" + this.password : "") + "@" : "")
            + this.host + this.pathname + this.search + this.hash;
    };
    global.URL.prototype.toString = function() { return this.href; };
    global.URL.prototype.toJSON = function() { return this.href; };

    global.Headers = function(init) {
        this._map = Object.create(null);
        if (init instanceof global.Headers)
            init = init._map;
        if (init && typeof init === "object") {
            if (Array.isArray(init))
                init.forEach(function(p) { this.append(p[0], p[1]); }, this);
            else
                for (var k in init) this.set(k, init[k]);
        }
    };
    global.Headers.prototype = {
        set: function(k, v) { this._map[String(k).toLowerCase()] = String(v); },
        append: function(k, v) {
            var key = String(k).toLowerCase();
            this._map[key] = this._map[key] ? this._map[key] + ", " + v : String(v);
        },
        get: function(k) {
            var v = this._map[String(k).toLowerCase()];
            return v === undefined ? null : v;
        },
        has: function(k) { return this.get(k) !== null; },
        "delete": function(k) { delete this._map[String(k).toLowerCase()]; },
        forEach: function(fn, self_) {
            for (var k in this._map) fn.call(self_, this._map[k], k, this);
        },
        keys: function() { return Object.keys(this._map); }
    };

    // AbortController: the signal is honoured by our fetch, which drops the
    // response rather than truly cancelling the transfer - the request is
    // already in flight in the UI process. Callers see the abort either way.
    global.AbortSignal = function() {
        this.aborted = false;
        this.reason = undefined;
        this._listeners = [];
        this.onabort = null;
    };
    global.AbortSignal.prototype = {
        addEventListener: function(type, fn) {
            if (type === "abort" && typeof fn === "function") this._listeners.push(fn);
        },
        removeEventListener: function(type, fn) {
            var i = this._listeners.indexOf(fn);
            if (i !== -1) this._listeners.splice(i, 1);
        },
        throwIfAborted: function() { if (this.aborted) throw this.reason; }
    };
    global.AbortController = function() { this.signal = new global.AbortSignal(); };
    global.AbortController.prototype.abort = function(reason) {
        var signal = this.signal;
        if (signal.aborted) return;
        signal.aborted = true;
        var error = new Error("The operation was aborted.");
        error.name = "AbortError";
        signal.reason = reason === undefined ? error : reason;
        var event = { type: "abort", target: signal };
        if (typeof signal.onabort === "function") {
            try { signal.onabort(event); } catch (e) {}
        }
        signal._listeners.slice().forEach(function(fn) {
            try { fn(event); } catch (e) {}
        });
    };

    global.TextEncoder = function() { this.encoding = "utf-8"; };
    global.TextEncoder.prototype.encode = function(input) {
        var utf8 = unescape(encodeURIComponent(String(input === undefined ? "" : input)));
        var out = new Uint8Array(utf8.length);
        for (var i = 0; i < utf8.length; i++) out[i] = utf8.charCodeAt(i);
        return out;
    };
    global.TextDecoder = function(encoding) { this.encoding = encoding || "utf-8"; };
    global.TextDecoder.prototype.decode = function(bytes) {
        if (!bytes) return "";
        var binary = "";
        var view = bytes.buffer ? new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength)
                                : new Uint8Array(bytes);
        for (var i = 0; i < view.length; i++) binary += String.fromCharCode(view[i]);
        try { return decodeURIComponent(escape(binary)); } catch (e) { return binary; }
    };

    global.performance = global.performance || { now: function() { return Date.now(); } };

    // Real entropy, injected by the host from /dev/urandom at context creation;
    // Math.random is not an acceptable source for anything calling itself
    // crypto. When the pool runs out we say so rather than quietly degrading.
    global.crypto = {
        getRandomValues: function(array) {
            var pool = global.__atlEntropy || "";
            for (var i = 0; i < array.length; i++) {
                if (global.__atlEntropyOffset + 2 <= pool.length) {
                    array[i] = parseInt(pool.substr(global.__atlEntropyOffset, 2), 16);
                    global.__atlEntropyOffset += 2;
                } else {
                    throw new Error("crypto.getRandomValues: entropy pool exhausted");
                }
            }
            return array;
        },
        randomUUID: function() {
            var b = global.crypto.getRandomValues(new Uint8Array(16));
            b[6] = (b[6] & 0x0f) | 0x40;
            b[8] = (b[8] & 0x3f) | 0x80;
            var hex = [];
            for (var i = 0; i < 16; i++) hex.push((b[i] + 0x100).toString(16).substr(1));
            return hex.slice(0, 4).join("") + "-" + hex.slice(4, 6).join("") + "-"
                 + hex.slice(6, 8).join("") + "-" + hex.slice(8, 10).join("") + "-"
                 + hex.slice(10, 16).join("");
        }
    };
    global.__atlEntropyOffset = 0;

    // fetch() over the UI process's QNetworkAccessManager. Only the subset
    // extensions actually use for list downloads: method, headers, body, and a
    // text/json/arrayBuffer-less response.
    var nextRequest = 1;
    var requests = Object.create(null);
    global.fetch = function(input, init) {
        init = init || {};
        var url = (typeof input === "string") ? input : (input && input.url);
        return new Promise(function(resolve, reject) {
            var id = nextRequest++;
            requests[id] = { resolve: resolve, reject: reject };
            global.__atlNative(JSON.stringify({ seq: 0, api: "net.fetch",
                args: [id, String(url), String(init.method || "GET"),
                       init.headers || {}, init.body === undefined ? null : String(init.body)] }));
        });
    };
    global.__atlFetchDone = function(id, ok, status, statusText, headers, body, error) {
        var entry = requests[id];
        if (!entry) return;
        delete requests[id];
        if (!ok) {
            entry.reject(new Error(error || "network error"));
            return;
        }
        entry.resolve({
            ok: status >= 200 && status < 300,
            status: status,
            statusText: statusText || "",
            url: "",
            headers: {
                get: function(name) {
                    var lower = String(name).toLowerCase();
                    for (var key in headers) {
                        if (key.toLowerCase() === lower) return headers[key];
                    }
                    return null;
                },
                has: function(name) { return this.get(name) !== null; }
            },
            text: function() { return Promise.resolve(body); },
            json: function() {
                try { return Promise.resolve(JSON.parse(body)); }
                catch (e) { return Promise.reject(e); }
            }
        });
    };

    // XMLHttpRequest for the many extensions that predate fetch.
    global.XMLHttpRequest = function() {
        var self = this;
        this.readyState = 0;
        this.status = 0;
        this.responseText = "";
        this.response = "";
        this._headers = {};
        this._method = "GET";
        this._url = "";
        this.onload = null;
        this.onerror = null;
        this.onreadystatechange = null;
        this.open = function(method, url) {
            self._method = method;
            self._url = url;
            self.readyState = 1;
        };
        this.setRequestHeader = function(name, value) { self._headers[name] = value; };
        this.getAllResponseHeaders = function() { return ""; };
        this.abort = function() {};
        this.send = function(body) {
            global.fetch(self._url, { method: self._method, headers: self._headers, body: body })
                .then(function(response) {
                    return response.text().then(function(text) {
                        self.status = response.status;
                        self.responseText = self.response = text;
                        self.readyState = 4;
                        if (self.onreadystatechange) self.onreadystatechange();
                        if (self.onload) self.onload();
                    });
                }, function(e) {
                    self.readyState = 4;
                    self.status = 0;
                    if (self.onreadystatechange) self.onreadystatechange();
                    if (self.onerror) self.onerror(e);
                });
        };
    };
})(typeof globalThis !== "undefined" ? globalThis : this);
)JS";

} // namespace WebExtensionScripts
