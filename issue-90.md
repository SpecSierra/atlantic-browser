# Issue 90 Handover

## Current state

Build 89.x regressed in four visible areas:

1. **App restart after close**
2. **Media volume slider has no effect**
3. **Text selection is broken**
4. **Video fullscreen exits immediately**

The bad speculative fixes were reverted back to the known-good baseline commit `7f19e705`, and one targeted fix was then applied for restart handling.

## What is already fixed

### App restart

- **Root cause:** removing the old close handling left no path from window close to `app->quit()`
- **Effect:** `app->exec()` never returned, `_exit()` was never reached, and the browser process stayed alive
- **Fix commit:** `38265f06`
- **Change:** connect the browser window `destroyed()` signal to `app->quit()` in `apps/browser/main.cpp`

Expected result: closing the app should fully terminate the process so the browser can be started again.

## What is not fixed yet

### 1. Volume

- C++ support exists in `apps/wpe/WPEWebPage.cpp` via `setMediaVolume()` and `setMediaMuted()`
- Those methods set HTML media element `volume` and `muted` state through injected JavaScript
- The remaining unknown is whether the visible slider is actually wired to those methods, or whether the failing case is specific to sites like YouTube

### 2. Text selection

- The long-press selection bridge still exists in `apps/wpe/WPEWebPage.cpp`
- Earlier attempts to “fix” scrolling by changing touch-end behavior made selection worse and were reverted
- Current state needs fresh device verification before touching the selection code again

### 3. Fullscreen

- Fullscreen request signals are still connected
- The reported behavior is: fullscreen appears briefly, then immediately exits, and video alignment is wrong during the short fullscreen period
- No root cause has been verified yet

## Important commits

- `5436a851` - compilation fixes and WebKit settings cleanup
- `5edd279f` - Qt 5.15 compatibility fix
- `7f19e705` - baseline after removing the problematic close signal path
- `38265f06` - proper app restart fix using window destruction handling

## Key files

- `apps/browser/main.cpp`
  - app lifecycle, quit path, `_exit()` handling
- `apps/wpe/WPEWebPage.cpp`
  - media bridge, selection bridge, fullscreen signal handling
- `apps/browser/qml/pages/BrowserPage.qml`
  - fullscreen UI and browser page behavior

## Verified lesson from issue 90

Blind fixes made the browser worse. The remaining issues should not be changed again without:

1. reproducing on device
2. watching logs during reproduction
3. adding only targeted instrumentation
4. confirming the exact failing path before changing behavior

## Recommended next steps

### First: verify restart fix on device

Use the phone directly and confirm:

- close browser
- confirm process exits
- relaunch browser
- confirm it starts normally

Suggested checks:

```bash
ssh -p 2222 defaultuser@localhost
journalctl -f
```

### Then diagnose the remaining three issues one at a time

#### Volume

- reproduce on a plain HTML5 video page and on YouTube
- determine whether the UI path reaches `setMediaVolume()`
- check whether site-specific controls bypass the browser bridge

#### Text selection

- verify long-press still fires
- verify `selectionchange` events still reach the native bridge
- avoid touching scroll heuristics until logs prove they are the cause

#### Fullscreen

- log `enterFullscreenRequested` and `leaveFullscreenRequested`
- verify whether fullscreen exit is being requested by WebKit or by browser UI logic
- inspect fullscreen geometry and centering path in QML

## Bottom line

Only the restart issue has an implemented fix right now. Volume, text selection, and fullscreen still require proper device-side diagnosis before any more code changes.
