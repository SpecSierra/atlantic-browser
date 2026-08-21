/****************************************************************************
**
** Copyright (c) 2014 - 2021 Jolla Ltd.
** Copyright (c) 2021 Open Mobile Platform LLC.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import QtQuick.Window 2.2 as QuickWindow
import Sailfish.Silica 1.0
import Sailfish.Browser 1.0
import Sailfish.WebView.Pickers 1.0 as Pickers
import Sailfish.WebView.Popups 1.0 as Popups
import Sailfish.WebView.Controls 1.0
import Sailfish.Policy 1.0
import Sailfish.TextLinking 1.0
import Nemo.Configuration 1.0
import "." as Browser

WebContainer {
    id: webView

    property bool activePortalMode
    readonly property bool moving: contentItem && contentItem.moving
    property bool portrait: true
    property bool contentFullscreen: contentItem && contentItem.fullscreen
    // needChrome is provided as FINAL by WPEWebContainer C++ (cannot override)
    property real fullscreenHeight
    property bool imOpened
    property real toolbarHeight
    property string favicon: contentItem ? contentItem.favicon : ""
    property bool findInPageHasResult: contentItem ? contentItem.findInPageHasResult : false
    property bool canShowSelectionMarkers: true

    property var resourceController: ResourceController {
        webPage: contentItem
        background: !webView.visible
    }

    onContentItemChanged: {
        resourceController._htmlAudioActive = contentItem ? contentItem.mediaAudioActive : false
        resourceController._htmlVideoActive = contentItem ? contentItem.mediaVideoActive : false
        resourceController.calculateStatus()

        // A preview belongs to the tab it was captured in: drop it, and the
        // URL-bar hold that goes with it, when the active page changes.
        if (typeof historyPreview !== "undefined" && historyPreview) {
            historyPreview.navigating = false
            historyPreview.dismiss()
        }
    }

    property var _webPageCreator: WebPageCreator {
        activeWebPage: contentItem
        model: tabModel
    }

    property Component _selectionUIComponent: Component {
        TextSelectionController {
            opacity: canShowSelectionMarkers ? 1.0 : 0.0
            contentWidth: webView.rotationHandler ? webView.rotationHandler.width : 0
            contentHeight: Math.max(0, webView.fullscreenHeight - webView.toolbarHeight)
            // Push below the overlay
            z: -1
            anchors {
                fill: parent
                bottomMargin: webView.toolbarHeight
            }

            Behavior on opacity { FadeAnimator {} }

            onStartHandleMaskChanged: browserPage.inputRegion.selectionStartHandleMask = startHandleMask
            onEndHandleMaskChanged: browserPage.inputRegion.selectionEndHandleMask = endHandleMask
        }
    }

    property var linkHandler: LinkHandler {}

    property QtObject fixedToolbarConfig: ConfigurationValue {
        key: "/apps/atlantic-browser/settings/fixed_toolbar"
        defaultValue: false

        onValueChanged: {
            // Re-show the toolbar if it was hidden when the setting got enabled.
            if (value && webView.contentItem && !webView.contentItem.chrome) {
                webView.contentItem.chrome = true
            }
        }
    }

    // Pages are created in C++ (WPEWebContainer::getOrCreatePage), not from
    // webPageComponent, so bindings inside that component never apply. Push
    // the fixed-toolbar setting onto whichever page is active.
    Binding {
        target: webView.contentItem
        property: "fixedToolbar"
        value: fixedToolbarConfig.value
        when: webView.contentItem !== null
    }

    Connections {
        target: contentItem
        ignoreUnknownSignals: true

        onMediaAudioActiveChanged: {
            resourceController._htmlAudioActive = contentItem ? contentItem.mediaAudioActive : false
            resourceController.calculateStatus()
        }
        onMediaVideoActiveChanged: {
            resourceController._htmlVideoActive = contentItem ? contentItem.mediaVideoActive : false
            resourceController.calculateStatus()
        }
    }

    function stop() {
        if (contentItem) {
            contentItem.stop()
        }
    }

    function clearSelection() {
        if (contentItem) {
            contentItem.clearSelection()
        }
    }

    function sendAsyncMessage(name, data) {
        if (!contentItem) {
            return
        }

        contentItem.sendAsyncMessage(name, data)
    }

    function thumbnailCaptureSize() {
        if (webView.activePortalMode) {
            console.log("Thumbnail size tried accessed in captive portal mode")
            return Qt.size(0, 0)
        }

        var ratio = Math.min(
                    browserPage.width / browserPage.thumbnailSize.width,
                    browserPage.height / browserPage.thumbnailSize.height)
        var width = browserPage.thumbnailSize.width * ratio
        var height = browserPage.thumbnailSize.height * ratio

        return Qt.size(width, height)
    }

    function grabActivePage() {
        if (webView.activePortalMode) {
            console.warn("Refusing page grab in active portal mode")
            return
        }

        if (webView.contentItem && webView.activeTabRendered) {
            if (webView.privateMode) {
                webView.contentItem.grabThumbnail(thumbnailCaptureSize())
            } else {
                webView.contentItem.grabToFile(thumbnailCaptureSize())
            }
        }
    }

    function handleKeyPress(key) {
        if (key == Qt.Key_F5) {
            reload()
        }
    }

    // foreground is owned by WPEWebContainer C++ (tracks the real window
    // visibility); the old binding here evaluated false at startup and never
    // re-fired (fakeVisibility has no notify), which kept pages hidden.
    readyToPaint: resourceController.videoActive ? webView.visible && !resourceController.displayOff
                                                 : webView.visible && webView.contentItem
                                                   && (webView.contentItem.domContentLoaded
                                                       || webView.contentItem.painted)

    touchBlocked: contentItem && contentItem.popupOpener && contentItem.popupOpener.active
                  || !AccessPolicy.browserEnabled || false

    onKeyPressed: handleKeyPress(key)

    onBackButtonPressed: webView.goBack()

    onForwardButtonPressed: webView.goForward()

    // ── History preview ("instant Back") ───────────────────────────────────
    // During a back/forward navigation the engine keeps presenting the OUTGOING
    // page's last frame until the incoming page paints, while the chrome
    // switches to the destination URL at once — so the viewport shows one page
    // and the URL bar names another. On edition.cnn.com that gap is the whole
    // load (DCL ~15 s, load ~24 s, measured 2026-08-19) because bfcache is
    // capacity 0 under ATLANTIC_CACHE_MODEL=viewer.
    //
    // Cover the gap with the destination entry's own last-seen pixels. This
    // buys no load time; it stops the browser showing a page it has left.
    Item {
        id: historyPreview

        // Cover exactly the live web content rect, not the whole container.
        // The page item is parented at the container's top-left and shortened
        // by the bottom inset (WPEWebContainer::insetPageHeight), so a preview
        // filling the container is taller than the pixels it replaces: with
        // PreserveAspectFit centring the shorter capture, the picture sat half
        // a toolbar too low and its bottom ran under the URL bar. Track the
        // page item instead — top-anchored, ending where the content does.
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
        }
        height: webView.contentItem ? webView.contentItem.height : parent.height
        clip: true                  // crop the surplus, never letterbox it

        z: 90
        opacity: 0
        visible: opacity > 0

        // Hard ceiling. If the incoming page never reports DOMContentLoaded
        // (dead network, blocked main document) the user must not be left
        // staring at a frozen screenshot forever.
        readonly property int maxHoldMs: 10000

        // The URL bar has to be on screen for the whole covered navigation:
        // the preview exists so that chrome and viewport agree, which is only
        // true if the chrome is actually visible. Held from the moment the
        // preview appears until the load ends — the picture is dismissed at
        // DOMContentLoaded, long before the page has finished.
        property bool navigating
        property bool holdingChrome
        // Remember which page was pinned: switching tabs mid-load must not
        // leave the outgoing page's toolbar forced open forever.
        property var chromeHeldPage: null
        readonly property bool wantsChrome: navigating || opacity > 0
        onWantsChromeChanged: setChromeHold(wantsChrome)

        function setChromeHold(hold) {
            if (hold === holdingChrome)
                return
            holdingChrome = hold
            if (hold) {
                chromeHeldPage = webView.contentItem
                if (chromeHeldPage)
                    chromeHeldPage.forceChrome(true)
                return
            }
            var page = chromeHeldPage
            chromeHeldPage = null
            // Find-in-page pins the toolbar through the same flag; releasing
            // here while it is up would drop its hold too.
            if (page && !page.findInPageHasResult)
                page.forceChrome(false)
        }

        function showPreview(path) {
            previewImage.source = "file://" + path
            navigating = true
            opacity = 1
            holdTimer.restart()
            chromeHoldGuard.restart()
        }

        function dismiss() {
            holdTimer.stop()
            opacity = 0
        }

        // Release the pixmap only once faded out, or the preview vanishes
        // instantly instead of fading.
        onOpacityChanged: {
            if (opacity === 0)
                previewImage.source = ""
        }

        Behavior on opacity { NumberAnimation { duration: 150 } }

        Image {
            id: previewImage

            anchors {
                left: parent.left
                right: parent.right
                top: parent.top    // glued to the top: never re-centre
            }
            // Fit the width and let the aspect decide the height, so the
            // capture is never squashed. It matches the viewport exactly in
            // the normal case; if the viewport changed between capture and
            // replay (rotation, toolbar inset, keyboard) the difference is
            // clipped off the bottom, where the user is not looking.
            height: sourceSize.width > 0
                    ? parent.width * sourceSize.height / sourceSize.width
                    : parent.height
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            cache: false            // one-shot, and the file is rewritten in place
        }

        // A touch means the user wants the live page, however unfinished. The
        // press is consumed rather than forwarded: the preview is a different
        // document from the one now loading, so replaying the tap into it would
        // hit whatever happens to sit at those coordinates.
        MouseArea {
            anchors.fill: parent
            enabled: historyPreview.opacity > 0
            onPressed: historyPreview.dismiss()
        }

        Timer {
            id: holdTimer
            interval: historyPreview.maxHoldMs
            onTriggered: historyPreview.dismiss()
        }

        // Ceiling on the URL-bar hold as well. The hold is normally released by
        // loadingChanged, but a load that never reports an end (killed process,
        // stalled main document) must not pin the toolbar for the session.
        Timer {
            id: chromeHoldGuard
            interval: 30000
            onTriggered: historyPreview.navigating = false
        }
    }

    // The load this preview covers has finished (or failed): stop pinning the
    // URL bar. Not a plain onLoadingChanged handler — BrowserPage.qml declares
    // one on this same instance, which would override it.
    Connections {
        target: webView
        onLoadingChanged: {
            if (!webView.loading)
                historyPreview.navigating = false
        }
    }

    Connections {
        target: webView.contentItem
        onHistoryPreviewReady: historyPreview.showPreview(imagePath)
        // First real milestone of the incoming document. Not first paint —
        // WPEWebPage.painted is a one-shot "has ever painted" flag and
        // QQuickWindow::frameSwapped counts window frames, not web frames, so
        // neither can gate this. DCL is the earliest per-navigation signal the
        // page already exposes.
        onDomContentLoadedChanged: {
            if (webView.contentItem && webView.contentItem.domContentLoaded)
                historyPreview.dismiss()
        }
    }

    // WPE selection drag handles (at container level so contentItem is available)
    Item {
        id: selHandles
        visible: webView.contentItem && webView.contentItem.textSelectionActive
        anchors.fill: parent
        z: 100

        property var ci: webView.contentItem
        property real dsf: ci ? ci.deviceScaleFactor || 1.0 : 1.0

        Item {
            id: selectionCursor
            visible: selHandles.visible && selHandles.ci && selHandles.ci.textSelectionActive
            x: selHandles.ci ? (selHandles.ci.selectionEndX * selHandles.dsf) : 0
            y: selHandles.ci ? (selHandles.ci.selectionEndY * selHandles.dsf) - (Theme.iconSizeMedium / 2) : 0
            width: Theme.paddingSmall
            height: Theme.iconSizeMedium
            z: 101

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 2
                height: parent.height
                radius: 1
                color: Theme.highlightColor
            }
        }

        Image {
            id: startHandle
            // Native Silica selection handle: teardrop with its tip at the
            // top-right, so the right edge / top sits on the selection start
            // (x below subtracts width) and the body hangs down-left, clear of
            // the selected text. Colorized to the ambience highlight colour.
            width: Theme.iconSizeMedium
            height: Theme.iconSizeMedium
            sourceSize.width: Theme.iconSizeMedium
            sourceSize.height: Theme.iconSizeMedium
            source: "image://theme/icon-m-textselection-start?" + Theme.highlightColor
            visible: selHandles.visible && selHandles.ci && selHandles.ci.selectionStartX >= 0
            x: selHandles.ci ? selHandles.ci.selectionStartX * selHandles.dsf - width : 0
            y: selHandles.ci ? selHandles.ci.selectionStartY * selHandles.dsf : 0

            MouseArea {
                anchors.fill: parent
                anchors.margins: -Theme.paddingLarge
                drag.target: startHandle
                drag.axis: Drag.XAndYAxis
                onPositionChanged: {
                    if (drag.active && selHandles.ci) {
                        var cssX = (startHandle.x + startHandle.width) / selHandles.dsf
                        var cssY = (startHandle.y + startHandle.height / 2) / selHandles.dsf
                        selHandles.ci.moveSelectionStart(cssX, cssY)
                    }
                }
            }
        }

        Image {
            id: endHandle
            // Native Silica selection handle: teardrop with its tip at the
            // top-left, so the left edge / top sits on the selection end and the
            // body hangs down-right, clear of the selected text. Colorized to
            // the ambience highlight colour.
            width: Theme.iconSizeMedium
            height: Theme.iconSizeMedium
            sourceSize.width: Theme.iconSizeMedium
            sourceSize.height: Theme.iconSizeMedium
            source: "image://theme/icon-m-textselection-end?" + Theme.highlightColor
            visible: selHandles.visible && selHandles.ci && selHandles.ci.selectionEndX >= 0
            x: selHandles.ci ? selHandles.ci.selectionEndX * selHandles.dsf : 0
            y: selHandles.ci ? selHandles.ci.selectionEndY * selHandles.dsf : 0

            MouseArea {
                anchors.fill: parent
                anchors.margins: -Theme.paddingLarge
                drag.target: endHandle
                drag.axis: Drag.XAndYAxis
                onPositionChanged: {
                    if (drag.active && selHandles.ci) {
                        var cssX = endHandle.x / selHandles.dsf
                        var cssY = (endHandle.y + endHandle.height / 2) / selHandles.dsf
                        selHandles.ci.moveSelectionEnd(cssX, cssY)
                    }
                }
            }
        }
    }

    webPageComponent: Component {
        WebPage {
            id: webPage

            property int frameCounter
            property bool rendered
            // textSelectionActive and textSelectionController are FINAL in WPEWebPage C++
            property Item _selectionUI: null
            readonly property bool activeWebPage: container.tabId == tabId
            property bool userHasDraggedWhileLoading

            property QtObject pickerOpener: Pickers.PickerOpener {
                pageStack: window.pageStack
                contentItem: webPage
            }

            property QtObject popupOpener: Popups.PopupOpener {
                pageStack: window.pageStack
                parentItem: browserPage
                contentItem: webPage
                // ContextMenu needs a reference to correct TabModel so that
                // private and public tabs are created to correct model. While context
                // menu is open, tab model cannot change (at least at the moment).
                tabModel: webView.tabModel

                onAboutToOpenContextMenu: {
                    if (Qt.inputMethod.visible) {
                        browserPage.focus = true
                        Qt.inputMethod.hide()
                    }

                    // Possible path that leads to a new tab. Thus, capturing current
                    // view before opening context menu.
                    if (!webView.activePortalMode) {
                        webView.grabActivePage()
                    }
                    contextMenuRequested(data)
                }

                onLoginSaved: {
                    if (!webView.activePortalMode) {
                        FaviconManager.grabIcon("logins", webPage,
                                                Qt.size(Theme.iconSizeMedium,
                                                        Theme.iconSizeMedium))
                    }
                }
            }

            signal selectionCopied(var data)
            signal contextMenuRequested(var data)

            function grabItem() {
                if (rendered && activeWebPage && active) {
                    if (webView.privateMode) {
                        grabThumbnail(thumbnailCaptureSize())
                    } else {
                        grabToFile(thumbnailCaptureSize())
                    }
                }
            }

            function clearSelection() {
                if (_selectionUI) {
                    _selectionUI.clearSelection()
                    browserPage.inputRegion.selectionStartHandleMask = Qt.rect(0, 0, 0, 0)
                    browserPage.inputRegion.selectionEndHandleMask = Qt.rect(0, 0, 0, 0)
                }
            }

            fixedToolbar: fixedToolbarConfig.value
            toolbarHeight: container.toolbarHeight
            throttlePainting: !foreground && !resourceController.videoActive && webView.visible || !webView.visible
            enabled: webView.enabled
            chromeGestureThreshold: toolbarHeight / 3
            chromeGestureEnabled: !forcedChrome && enabled && !webView.imOpened && !fixedToolbar

            onFileGrabWritten: tabModel.updateThumbnailPath(tabId, fileName)

            // Image data is base64 encoded which can be directly used as source in Image element
            onThumbnailResult: tabModel.updateThumbnailPath(tabId, data)

            onAtYBeginningChanged: {
                if (atYBeginning && activeWebPage && domContentLoaded) {
                    chrome = true
                }
            }

            onAtYEndChanged: {
                // Don't hide chrome if content length is short i.e. forcedChrome is enabled.
                if (!atYBeginning && atYEnd && !forcedChrome && !fixedToolbar && chrome
                        && activeWebPage && domContentLoaded) {
                    chrome = false
                }
            }

            onUrlChanged: {
                if (url == "about:blank")
                    return

                // findInPageHasResult is bound to contentItem; C++ resets it on new navigation
                var modelUrl = tabModel.url(tabId)

                rendered = false
                frameCounter = 0

                // If url has changed or url doesn't exist in the model,
                // clear the thumbnail. Preserve the thumbnails in the model
                // if it has the same url (restarting browser / resurrecting a tab).
                if (!modelUrl || modelUrl != url) {
                    tabModel.updateThumbnailPath(tabId, "")
                }
            }

            // onBackgroundColorChanged not available in WPEWebPage
            // onBackgroundColorChanged: {
            //     if (container.contentItem === webPage) {
            //         sendAsyncMessage("Browser:SelectionColorUpdate",
            //                          { "color": Theme.secondaryHighlightColor })
            //     }
            // }

            onDraggingChanged: {
                if (dragging && loading) {
                    userHasDraggedWhileLoading = true
                }
            }

            onLoadedChanged: {
                if (loaded) {
                    if (!userHasDraggedWhileLoading && resurrectedContentRect) {
                        sendAsyncMessage("embedui:zoomToRect",
                                         {
                                             "x": resurrectedContentRect.x, "y": resurrectedContentRect.y,
                                             "width": resurrectedContentRect.width, "height": resurrectedContentRect.height
                                         })
                        resurrectedContentRect = null
                    }

                    if (!webView.activePortalMode) {
                        grabItem()
                        // Favicons are grabbed in C++ (WPEWebContainer::
                        // onPageFaviconChanged): handlers in this component
                        // never run, see the webPageComponent note above.
                    }
                }

                // Refresh timers (if any) keep working even for suspended views. Hence
                // suspend the view again explicitly if browser content window is in not visible (background).
                if (loaded && !webView.visible) {
                    suspendView()
                }
            }

            onLoadingChanged: {
                if (loading) {
                    userHasDraggedWhileLoading = false
                    webPage.chrome = true
                }
            }

            onAfterRendering: {
                // Try to capture something else than glClear color.
                if (frameCounter < 3) {
                    ++frameCounter
                } else if (!rendered) {
                    rendered = true
                    if (!webView.activePortalMode) {
                        grabItem()
                    }
                }
            }

            onRecvAsyncMessage: {
                if (pickerOpener.message(message, data) || popupOpener.message(message, data)) {
                    return
                }

                switch (message) {
                case "Link:SetIcon": {
                    // Legacy Gecko path, never delivered under WPE. Icons come
                    // from the faviconBridge user script instead, which drives
                    // WPEWebPage::favicon / faviconCandidates directly.
                    break
                }
                case "Content:SelectionRange": {
                    if (_selectionUI === null) {
                        _selectionUI = _selectionUIComponent.createObject(browserPage,
                                                                                                {"contentItem": webPage})
                    }
                    _selectionUI.selectionRangeUpdated(data)
                    break
                }
                case "Content:SelectionSwap": {
                    if (_selectionUI) {
                        _selectionUI.swap()
                    }

                    break
                }
                case "embed:find": {
                    // Dead code in WPE — Gecko find handler, kept for reference
                    break
                }
                // embed:OpenLink listener is registered only in the captive portal mode
                case "embed:OpenLink": {
                    linkHandler.handleLink(data.uri)
                    break
                }
                case "Link:AddSearch": {
                    if (!webView.privateMode) {
                        // This adds this search as available if not already there
                        SearchEngineModel.add(data.engine.title, data.engine.href)
                    }
                    break
                }
                }
            }
            onContextMenuRequested: {
                if (data.types.indexOf("content-text") !== -1) {
                    // we want to select some content text
                    webPage.sendAsyncMessage("Browser:SelectionStart", {"xPos": data.xPos, "yPos": data.yPos})
                }
            }

            Component.onCompleted: {
                console.log("[QML-STARTUP] WebPage component created, tabId=" + tabId)
                addMessageListener("Content:SelectionRange")
                addMessageListener("Content:SelectionCopied")
                addMessageListener("Content:SelectionSwap")
            }
        }
    }
}
