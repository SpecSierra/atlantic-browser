# atlantic-browser

Sailfish Silica Qt/QML browser UI for **Atlantic Browser**, backed by a custom
WPE WebKit Qt5 bridge. The engine, patch stack, packaging and CI live in the
companion repo `SpecSierra/atlantic-engine`; the two ship together.

| | |
|---|---|
| Sailfish OS target | **5.1.0.11** (Pispala) |
| Qt | **5.6.3**, C++17, qmake |
| WPE WebKit | **2.52.6** via `wpewebkit-2.0` / `wpe-1.0` |
| Package | `atlantic-browser` **1.1.0** (set by `ATLANTIC_BROWSER_VERSION` in atlantic-engine `versions.env`; `rpm/sailfish-browser.spec` is the unused upstream spec) |
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
| Shared library `libsailfishbrowser.so` | `WPEWebContainer` (tab lifecycle), `WPEWebPage` (Qt↔WPE), `BrowserService` (D-Bus), `DownloadManager`, `FaviconManager`, `SettingManager`, `BookmarkManager` |
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
| `imageLongPressBridge` | image URL on long-press, for the context menu |
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
| `apps/wpe/` | WPE bridge — `WPEWebPage`, `WPEWebContainer`, `WPEUserScripts.h`, `AdBlockEngine`, `WPERuntimePaths` |
| `apps/lib/` | shared-library bootstrap (`browserruntime.cpp`) |
| `apps/core/` | `Browser`, `DownloadManager`, `FaviconManager`, `SettingManager` |
| `apps/history/`, `apps/storage/` | tab/history models; SQLite backend (`DBManager`, `DBWorker`, `Tab`, `Link`) |
| `apps/factories/` | page factory stubs |
| `settings/` | Sailfish Settings plugin |
| `data/` | prefs, search engines, launcher icon |
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

## License

MPL-2.0 — see `LICENSE.txt`.
