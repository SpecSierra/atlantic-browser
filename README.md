# Atlantic Browser

Atlantic Browser is the maintained Sailfish OS browser port. It uses the Sailfish Silica Qt browser UI backed by a WPE WebKit 2.52.4 Qt5 bridge.

Maintainer: [SpecSierra](https://github.com/SpecSierra)

---

## Architecture

The browser is split into four layers:

```
┌────────────────────────────────────────────┐
│  QML UI Layer (Sailfish Silica)             │
│  BrowserPage → WebView → Overlay/ToolBar    │
│  TabView, Settings, Bookmarks, History       │
├────────────────────────────────────────────┤
│  Shared Library (libsailfishbrowser.so)     │
│  ┌──────────────────────────────────────┐   │
│  │ WPEWebContainer  — tab lifecycle     │   │
│  │ WPEWebPage       — Qt↔WPE rendering │   │
│  │ BrowserService   — DBus API          │   │
│  │ DownloadManager  — TransferEngine    │   │
│  │ FaviconManager   — icon cache        │   │
│  │ SettingManager   — dconf settings    │   │
│  │ BookmarkManager  — bookmark CRUD     │   │
│  └──────────────────────────────────────┘   │
├────────────────────────────────────────────┤
│  Storage Layer                              │
│  DBManager → DBWorker → SQLite              │
│  PersistentTabModel, DeclarativeHistoryModel│
├────────────────────────────────────────────┤
│  Engine Layer (external)                    │
│  WPEQtView ← WPE WebKit 2.52.4             │
│  libWPEWebKit-2.0, libwpe-1.0              │
└────────────────────────────────────────────┘
```

### WPE WebKit Qt5 Bridge (`apps/wpe/`)

The core rendering bridge is `WPEWebPage`, a `QQuickItem` subclass (via `WPEQtView`) that hosts a `WebKitWebView`. Frames rendered by the WPE WebProcess are posted to the Qt Quick scene graph via `QQuickWindow::update()` — rendering is demand-driven, not a 60fps loop. A 2-second safety watchdog timer (`m_framePump`) ensures the compositor doesn't stall in edge cases.

**Key C++ classes:**

| Class | Role |
|-------|------|
| `WPEWebPage` | Core web page widget. URL loading, scroll, fullscreen, media, text selection, pinch zoom, find-in-page, security info, file choosers, downloads. ~40 Q_PROPERTY values exposed to QML. |
| `WPEWebContainer` | Tab manager. Creates/activates/destroys `WPEWebPage` instances. Maps tab IDs to pages. Configures sandbox paths and WebProcess memory limits. |
| `WPEWebPageCreator` | Thin stub for QML `WebPageCreator` compatibility. |

**Five JS bridges** are injected into every page via `WebKitUserContentManager`:

| Bridge | Purpose |
|--------|---------|
| `selectBridge` | Intercepts `<select>` touch events, sends options to native `ContextMenu` |
| `selectionBridge` | Throttled text selection reporter — coordinates for draggable selection handles |
| `scrollBridge` | Throttled scroll position reporter — drives chrome show/hide gesture |
| `mediaBridge` | Media playback state, volume sync between native slider (DBus `com.Meego.MainVolume2`) and page elements |
| `imageLongPressBridge` | Image URLs on long-press for context menu |

**Input pipeline:** Touch events dispatch to WPE directly. Pinch zoom tracks two-finger gestures via `visualScale` Q_PROPERTY. Virtual keyboard input is dispatched via injected JS (`dispatchTextToFocusedElement`, `dispatchBackspaceToFocusedElement`) rather than WebKit's built-in input method.

### QML UI (`apps/browser/qml/`)

The UI is built with Sailfish Silica components.

| Component | Role |
|-----------|------|
| `browser.qml` | Entry point — `BrowserWindow` with `BrowserPage` as `initialPage` |
| `BrowserPage.qml` | Main page (769 lines): `Overlay`, `WebView`, input regions, file pickers, select menu |
| `WebView.qml` | Wraps `WPEWebContainer`. Creates `WebPage` dynamically. JS bridge message handler. |
| `Overlay.qml` | Toolbar + URL/search bar overlay |
| `OverlayAnimator.qml` | State machine: `chromeVisible`, `fullscreenWebPage`, `startPage`, `secondaryTools`, `draggingOverlay`, `certOverlay`, `noOverlay` |
| `ResourceController.qml` | Audio/video resource lifecycle. Listens to MCE DBus for screen blank/unblank. Calls `suspendView()`/`resumeView()`. |

### DBus Services (`apps/browser/`)

Two DBus services are registered, both implemented by `BrowserService`/`BrowserUIService`:

| Service | Methods |
|---------|---------|
| `org.atlantic.browser` | `openUrl`, `activateNewTabView`, `cancelTransfer`, `restartTransfer`, `dumpMemoryInfo` |
| `org.atlantic.browser.ui` | `openUrl`, `openSettings`, `requestTab`, `closeTab`, `showChrome` |

External DBus inputs consumed:
- `com.Meego.MainVolume2` — volume slider state (polled every 500ms)
- `com.nokia.mce` — screen blank/unblank signals

### Storage (`apps/storage/`, `apps/history/`)

SQLite-backed via `DBManager` (singleton) and `DBWorker` (runs on a dedicated `QThread`):

| Model | Backing |
|-------|---------|
| `PersistentTabModel` | SQLite — persisted tab state |
| `PrivateTabModel` | In-memory only |
| `DeclarativeHistoryModel` | SQLite — browsing history |
| `DeclarativeBookmarkModel` | SQLite — bookmarks |
| `DeclarativeLoginModel` | SQLite — saved credentials |

### Shared Library and Bootstrap (`apps/lib/`)

`libsailfishbrowser.so` is loaded at runtime by `main.cpp`. The entry point `atlanticBrowserRuntimeStart()` creates the `Browser` orchestrator, connects DBus services, and loads QML into the `QQuickView`.

At startup, `main.cpp` probes GPU capabilities (EGL/GLES extensions) via an offscreen `QOpenGLContext` and sets `ATLANTIC_GPU_CONSERVATIVE` if GLES < 3 or external image extension is missing.

---

## Directory Map

| Path | Contents |
|------|----------|
| `apps/browser/` | `main.cpp`, DBus services, bookmarks, login models, QML pages |
| `apps/browser/qml/` | Browser QML UI — `BrowserPage`, `Overlay`, `ToolBar`, `TabView`, `FavoriteGrid`, etc. |
| `apps/shared/` | Reusable QML — `WebView`, `BrowserWindow`, `OverlayAnimator`, `ResourceController` |
| `apps/wpe/` | WPE bridge — `WPEWebPage`, `WPEWebContainer`, `WPERuntimePaths` |
| `apps/lib/` | Shared library bootstrap — `browserruntime.cpp` |
| `apps/core/` | Core utilities — `Browser`, `DownloadManager`, `FaviconManager`, `SettingManager` |
| `apps/history/` | Tab and history models |
| `apps/storage/` | SQLite backend — `DBManager`, `DBWorker`, `Tab`, `Link` |
| `apps/factories/` | Page factory stubs |
| `settings/` | Sailfish Settings plugin |
| `rpm/` | RPM spec (`sailfish-browser.spec`) |
| `data/` | Content blocker JSON, prefs, app icon |

---

## Build

Build system: **qmake** (Qt 5.15.13), C++17, targeting `aarch64-linux-gnu`.

```
qmake sailfish-browser.pro
make
```

Key `pkg-config` dependencies: `wpewebkit-2.0`, `wpe-1.0`, `Qt5Core/Qml/Quick/Gui/DBus/Concurrent/Sql`, `nemotransferengine-qt5`, `mlite5`, `sailfishpolicy`, `dsme_dbus_if`, `glib-2.0`, `gio-2.0`.

Build and packaging are handled by CI in the companion repo [atlantic-engine](https://github.com/SpecSierra/atlantic-engine).

---

## License

MPL 2.0 — see `LICENSE.txt`.

Repository: [github.com/SpecSierra/atlantic-browser](https://github.com/SpecSierra/atlantic-browser)
