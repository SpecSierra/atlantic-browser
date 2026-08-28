# atlantic-browser

Sailfish Silica Qt/QML browser UI for **Atlantic Browser**, backed by a custom
WPE WebKit Qt5 bridge. The engine, patch stack, packaging and CI live in the
companion repo `SpecSierra/atlantic-engine`; the two ship together.

| | |
|---|---|
| Sailfish OS target | **5.1.0.11** (Pispala) |
| Qt | **5.6.3**, C++17, qmake |
| WPE WebKit | **2.52.6** via `wpewebkit-2.0` / `wpe-1.0` |
| Package | `atlantic-browser` **1.3.0** (set by `ATLANTIC_BROWSER_VERSION` in atlantic-engine `versions.env`; `rpm/sailfish-browser.spec` is the unused upstream spec) |
| Builds | CI in `atlantic-engine` — push to `master` there |
| Maintainer | [SpecSierra](https://github.com/SpecSierra) |

## Documentation

Shared docs live in the engine repo, since most work spans both:

| Doc | Read it when |
|---|---|
| `atlantic-engine/docs/BUILD.md` | building, packaging, CI, version pins |
| `atlantic-engine/docs/DEVICE.md` | deploy, launch, screenshots, touch, inspector, `atldbg` |
| `atlantic-engine/docs/BENCHMARKING.md` | measuring a change or running an A/B |
| `atlantic-engine/docs/investigations/` | why something is the way it is; which theories are dead |

## Architecture

| Layer | Contents |
|---|---|
| QML UI (Sailfish Silica) | `BrowserPage` → `WebView` → `Overlay`/`ToolBar`; `TabView`, settings, bookmarks, history |
| Shared library `libsailfishbrowser.so` | `WPEWebContainer` (tab lifecycle), `WPEWebPage` (Qt↔WPE), `WebExtensionManager` (extension host), `BrowserService` (D-Bus), `DownloadManager`, `FaviconManager`, `SettingManager`, `BookmarkManager` |
| Storage | `DBManager` → `DBWorker` → SQLite; `PersistentTabModel`, `DeclarativeHistoryModel` |
| Engine (external) | `WPEQtView` ← WPE WebKit 2.52.6, `libWPEWebKit-2.0`, `libwpe-1.0` |

### WPE bridge (`apps/wpe/`)

`WPEWebPage` is a `QQuickItem` (via `WPEQtView`) hosting a `WebKitWebView`. Frames
from the WPE WebProcess are posted to the Qt Quick scene graph with
`QQuickWindow::update()` — demand-driven, not a 60 fps loop; a 2 s watchdog
(`m_framePump`) covers the edge cases where the compositor would otherwise stall.

| Class | Role |
|---|---|
| `WPEWebPage` | the page: URL loading, scroll, fullscreen, media, text selection, pinch zoom, find-in-page, security info, file choosers, downloads. ~40 `Q_PROPERTY` values exposed to QML |
| `WPEWebContainer` | tab manager — creates/activates/destroys pages, maps tab IDs, configures sandbox paths, WebProcess memory limits and tab discarding |
| `AdBlockEngine`, `AdBlockListUpdater` | Brave/Rust blocker wrapper and the filter-list refresh |
| `WPEWebPageCreator` | stub for QML `WebPageCreator` compatibility |

**JS bridges** injected via `WebKitUserContentManager` and handled in `WebView.qml`:

| Bridge | Purpose |
|---|---|
| `selectBridge` | intercepts `<select>` taps, renders options as a native `ContextMenu` |
| `selectionBridge` | throttled text-selection reporter — coordinates for draggable handles |
| `scrollBridge` | throttled scroll position — drives chrome show/hide |
| `mediaBridge` | playback state; volume sync with the native slider (`com.Meego.MainVolume2`) |
| `imageLongPressBridge` | long-press target — image URL, link URL, editable state and selection; drives the long-press panel and `contextMenus` item matching |
| `pinchBridge` | reports whether the page handled a pinch, so the browser gesture can stand down |
| `inputPickerBridge` | HTML5 date/time/color inputs → Silica pickers |
| `loginBridge` | password capture and gesture-first autofill |
| `editableFocusBridge` | runs inside cross-origin subframes to report editable focus, which the main-frame probe cannot see — keeps the keyboard up for embedded payment/comment fields |

**Site quirks and perf scripts** live in `apps/wpe/WPEUserScripts.h` (extracted
verbatim from `WPEWebPage.cpp` to keep it manageable): `kYouTubeH264`,
`kYouTubeIconFix`, `kTwitch`, `kMgpNativeHls`, `kIconHeal`, `kRedditPerf`,
`kPerfCss`, `kPassiveScroll`, `kMediaBufferCap`, `kCookieScrollUnlock`,
`kAdblockClassIdCollector`, plus the autoconsent and chrome-reveal scripts. Each
carries a comment explaining the defect it works around — read it before editing,
and validate changes through the Web Inspector before pushing to CI.

**Input:** touch events dispatch to WPE directly. Pinch zoom is page-driven with the
browser gesture as fallback (`visualScale`). Virtual-keyboard text goes in through
injected JS (`dispatchTextToFocusedElement`, `dispatchBackspaceToFocusedElement`),
except Enter, which needs the native keysym path; the keyboard height is reserved in
the web viewport via the container's bottom inset.

### WebExtensions (`apps/wpe/WebExtension*`)

Atlantic runs MV2 and the practical subset of MV3, unpacked or from a `.zip` /
`.xpi` / `.crx`, out of
`~/.local/share/org.sailfishos/browser/extensions/<id>/`. All of it is UI-process
work on APIs WPE WebKit 2.52 already exposes — the engine repo carries no
extension code. Design record, limits and the device-verification plan:
`atlantic-engine/docs/investigations/webextensions.md`.

| Class | Role |
|---|---|
| `WebExtension` | manifest model, `MatchPattern`, `_locales` |
| `WebExtensionManager` | registry, `atlantic-extension://` scheme handler, content-script install, `browser.*` dispatch; also the `QAbstractListModel` the settings UI lists |
| `WebExtensionBackgroundView` | background page in a hidden `WebKitWebView` |
| `WebExtensionBackground` | `JSCContext` background host — the fallback, forced with `ATLANTIC_EXT_BACKGROUND_PAGE=0` |
| `WebExtensionArchive` | central-directory zip reader (`QZipReader` cannot read AMO's streamed zips) |
| `WebExtensionStore` | addons.mozilla.org search and install; nothing mirrored, nothing recommended |
| `WebExtensionCookies` | `browser.cookies` over the default session's `WebKitCookieManager` |
| `WebExtensionBrowsingData` | `browser.history` and `browser.bookmarks` over `DBManager` and the live bookmark model |
| `WebExtensionDownloads` | `browser.downloads` over `DownloadManager`'s per-download records |
| `WebExtensionScripts.h` | the whole `browser.*` / `chrome.*` JS shim and the background polyfills — the biggest and riskiest file in the set |

**Isolation:** each extension's content scripts go in via
`webkit_user_script_new_for_world()` into their own world, `atlantic-ext-<id>`;
Chrome match patterns pass to WebKit unchanged except `<all_urls>`, which is
expanded. Extension pages (popup, options) are ordinary tabs on
`atlantic-extension://<id>/…` and get a *second* handler in the default world,
allow-listed to their own origin. The scheme is registered secure and
CORS-enabled, with a path-traversal guard and `web_accessible_resources`
enforced in the handler.

**Bridge:** JS never calls C++ directly. Pages post `{seq, api, args}` on
`window.webkit.messageHandlers.atlExt_<id>`; the background context calls
`__atlNative(json)`; replies come back as `__atlExtBridge.dispatch(...)`
evaluated in the right world. Promise/callback duality, `Event` objects, ports,
storage areas, `i18n` and `alarms` all live in the JS shim, so C++ sees flat
calls. Tab APIs are `WPEWebContainer` implementing `WebExtensionHost`.

**Background pages run in a real hidden web view**, on a non-EGL
`wpe_view_backend_exportable_fdo` whose frames are released and acked
immediately — a Firefox MV3 background is an event page with a DOM, and under
bare JSC its handlers silently never settled. Two traps that cost device time:
creating any `wpe_view_backend` before a real web view exists aborts at startup
(background pages are gated behind `setEngineReady()`), and a view must be
`webkit_web_view_terminate_web_process()`'d before unref or every reload orphans
a WebProcess per extension.

**Browser data:** `cookies` runs on the default network session's
`WebKitCookieManager` (`WebExtensionCookies.cpp`) — one store, `"0"`, since
private tabs are on an ephemeral session that is deliberately not exposed, and
`onChanged` fires only for changes made through the API because WebKit reports
none of its own. `history` and `bookmarks` (`WebExtensionBrowsingData.cpp`) go
through the browser's own subsystems: `DBWorker::searchHistory()`, added for
this, is tagged per request and keeps the visit count and timestamp that
`getHistory()` drops; bookmarks are written through the live
`DeclarativeBookmarkModel` so the UI updates immediately, and its flat list is
presented as one folder under a synthesised root. Both are gated on their
permission, cookies additionally on host access.

`downloads` runs on `DownloadManager`, which grew a per-download record for it
(the transfer engine owns the UI and cannot answer `search()`); records are
per-session. An API-initiated download settles its own destination instead of
raising the Save As prompt, `filename` must be a bare name so it cannot escape
the download folder, and `pause`/`resume` reject — WebKit's download API has no
pause.

**Deliberately inert** — the call rejects with a message and `addListener` is
accepted silently, because extensions register these at top level and throwing
kills the whole background script: `webRequest` and `declarativeNetRequest`
(network blocking is owned by the Rust WebProcess extension, which the UI
process cannot hand a per-request veto to), `proxy`, `idle`,
`management` beyond `getSelf`, and the MV3 service-worker lifecycle.
`scripting`, `contextMenus`, `runtime.onInstalled`, `webNavigation` and
`notifications` *are* implemented. Whatever an installed extension asks for that
we do not do is surfaced as a per-extension warning in Settings → Extensions.

That inert list has a consequence worth stating plainly: the popular end of the
ecosystem is webRequest-based, so uBlock Origin, Tampermonkey, Violentmonkey and
Stylus are **broken** here, and the store says so on the row before you install
one. Verdicts are derived, not guessed — AMO reports declared permissions before
download, so `WebExtensionStore::verdictFor()` classifies every search result —
and only a bad verdict is ever shown: an add-on with nothing against it says
nothing, since "works" would be a promise nobody has tested. Nothing is
mirrored and nothing is suggested; packages come from AMO and are checked
against the `sha256:` digest AMO publishes, except the paste-an-`.xpi`-URL
escape hatch, which the dialog says is unchecked.

**Tests** (host, no device): `node tests/webextension-shim.test.js` and
`python3 tests/test_extension_store.py` (`ATLANTIC_STORE_ONLINE=1` re-derives
verdicts from what AMO declares today — the guard against the rule drifting). `tests/sample-extension/` is the
on-device smoke test. Background pages appear as their own inspectable target;
an extension's JSC background context answers plain `Runtime.evaluate`, not the
Target-wrapped protocol.

### QML UI (`apps/browser/qml/`, `apps/shared/`)

| Component | Role |
|---|---|
| `browser.qml` | entry point — `BrowserWindow` with `BrowserPage` as `initialPage` |
| `BrowserPage.qml` | main page (~800 lines): overlay, web view, input regions, file pickers, select menu |
| `WebView.qml` | wraps `WPEWebContainer`, creates pages dynamically, routes bridge messages |
| `Overlay.qml` | toolbar plus URL/search bar |
| `OverlayAnimator.qml` | state machine: `chromeVisible`, `fullscreenWebPage`, `startPage`, `secondaryTools`, `draggingOverlay`, `certOverlay`, `noOverlay` |
| `ResourceController.qml` | audio/video lifecycle; listens to MCE D-Bus for screen blank and calls `suspendView()`/`resumeView()` |
| `Background.qml` | screen-fixed blurred ambience wallpaper (sampled in the shader via `gl_FragCoord`) |
| `ExtensionsPage.qml` | installed extensions — enable/remove, per-extension warnings for unsupported APIs, install from file |
| `ExtensionStorePage.qml` | AMO search and install; warns on a row that will not work, and says nothing on one that will |
| `pages/components/ImageActionPanel.qml` | the general long-press panel — built-in actions plus matching `contextMenus` items |

Two QML gotchas that have cost time: `webPageComponent` is dead — pages are created
in C++, so per-page settings must be pushed with a `Binding` on `contentItem`. And
`MDConfItem` is a no-op stub; use a QML `ConfigurationValue` or a C++ invokable.

### D-Bus

| Service | Methods |
|---|---|
| `org.atlantic.browser` | `openUrl`, `activateNewTabView`, `cancelTransfer`, `restartTransfer`, `dumpMemoryInfo` |
| `org.atlantic.browser.ui` | `openUrl`, `openSettings`, `requestTab`, `closeTab`, `showChrome` |

Consumed: `com.Meego.MainVolume2` (volume, polled every 500 ms) and `com.nokia.mce`
(screen blank/unblank). Sailjail silently drops any D-Bus name not on the
allowlist — add it to the RPM `Permissions` line, or the call just never arrives.

### Storage (`apps/storage/`, `apps/history/`, `apps/browser/settings/`)

SQLite via `DBManager` (singleton) and `DBWorker` (dedicated `QThread`).

| Model | Backing |
|---|---|
| `PersistentTabModel` | SQLite — persisted tabs |
| `PrivateTabModel` | in-memory only |
| `DeclarativeHistoryModel` | SQLite — history (skipped in private mode) |
| `DeclarativeBookmarkModel` | SQLite — bookmarks |
| `DeclarativeLoginModel` | the password vault — `CredentialStore` over SQLCipher, unlocked by a master password |
| `SearchEngineModel` | `data/searchEngines/` |

### Bootstrap (`apps/lib/`)

`main.cpp` loads `libsailfishbrowser.so` at runtime; `atlanticBrowserRuntimeStart()`
creates the `Browser` orchestrator, connects the D-Bus services, and loads QML into
the `QQuickView`. Startup probes EGL/GLES via an offscreen `QOpenGLContext` and sets
`ATLANTIC_GPU_CONSERVATIVE` when GLES < 3 or the external-image extension is
missing. The runtime is loaded on the first `afterRendering` so the UI paints first.

## Layout

| Path | Contents |
|---|---|
| `apps/browser/` | `main.cpp`, D-Bus services, bookmarks, login/search models, QML pages |
| `apps/browser/qml/` | `BrowserPage`, `Overlay`, `ToolBar`, `TabView`, `FavoriteGrid`, `SettingsPage`, … |
| `apps/shared/` | reusable QML — `WebView`, `BrowserWindow`, `OverlayAnimator`, `ResourceController`, `Background` |
| `apps/wpe/` | WPE bridge — `WPEWebPage`, `WPEWebContainer`, `WPEUserScripts.h`, `AdBlockEngine`, `WPERuntimePaths`; extension host — `WebExtension*` |
| `apps/lib/` | shared-library bootstrap (`browserruntime.cpp`) |
| `apps/core/` | `Browser`, `DownloadManager`, `FaviconManager`, `SettingManager` |
| `apps/history/`, `apps/storage/` | tab/history models; SQLite backend (`DBManager`, `DBWorker`, `Tab`, `Link`) |
| `apps/factories/` | page factory stubs |
| `settings/` | Sailfish Settings plugin |
| `data/` | prefs, search engines, launcher icon |
| `tests/` | host-side tests — extension shim, store verdict rules, `sample-extension/` |
| `translations/`, `rpm/` | translation project files (`.ts` are generated, not tracked); RPM spec |

## Build

Built by CI in `atlantic-engine` — never build production locally. For a local
syntax/compile check against the staged engine prefix:

```sh
qmake sailfish-browser.pro && make
```

`pkg-config` deps: `wpewebkit-2.0`, `wpe-1.0`, `Qt5Core/Qml/Quick/Gui/DBus/Concurrent/Sql`,
`nemotransferengine-qt5`, `mlite5`, `sailfishpolicy`, `dsme_dbus_if`, `glib-2.0`,
`gio-2.0`. QML under `/usr/share` can be hot-deployed to the device without a rebuild.

## Working rules

- **Reproduce on device before changing UI behaviour** — no behaviour change on a theory.
- **A feature touches every layer in one pass** — engine C++, QML/UI, settings
  storage, sailjail/D-Bus permissions. List the layers and confirm the list before
  implementing; finish with an on-device check that the user-visible behaviour works.
- **Ship engine-side optimizations behind an env flag, default OFF**, then a 5×5
  interleaved on-device A/B before flipping the default.
- **Commit to `main`** (this repo; the engine repo uses `master`), no feature
  branches; push only when asked.

## Upstream

Atlantic Browser is a fork of [sailfishos/sailfish-browser](https://github.com/sailfishos/sailfish-browser)
(Jolla Ltd. / Open Mobile Platform LLC, MPL-2.0). Most of the QML UI, the storage
and history models and the D-Bus plumbing originate there; the WPE bridge in
`apps/wpe/` replaces the original Gecko/EmbedLite one and is our own.

When a change is ported from upstream, keep the attribution intact:

- retain (and extend, if the upstream change is newer) the copyright header of
  the file, and note in it which upstream commit the code came from;
- credit the upstream author with a `Co-authored-by:` trailer in the commit, and
  link the upstream commit with an `Origin:` trailer.

## License

MPL-2.0 — see `LICENSE.txt`. The same licence as upstream, so code moves between
the two projects freely as long as the notices above travel with it.
