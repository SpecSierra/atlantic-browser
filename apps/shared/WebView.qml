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

    property QtObject fixedToolbarConfig: QtObject {
        property bool value: false
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

    foreground: visibility >= QuickWindow.Window.Maximized && Qt.application.state === Qt.ApplicationActive
    readyToPaint: resourceController.videoActive ? webView.visible && !resourceController.displayOff
                                                 : webView.visible && webView.contentItem
                                                   && (webView.contentItem.domContentLoaded
                                                       || webView.contentItem.painted)

    touchBlocked: contentItem && contentItem.popupOpener && contentItem.popupOpener.active
                  || !AccessPolicy.browserEnabled || false

    onKeyPressed: handleKeyPress(key)

    onBackButtonPressed: webView.goBack()

    onForwardButtonPressed: webView.goForward()

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

        Rectangle {
            id: startHandle
            width: Theme.iconSizeExtraSmall
            height: Theme.iconSizeMedium
            radius: width / 2
            color: Theme.highlightColor
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

        Rectangle {
            id: endHandle
            width: Theme.iconSizeExtraSmall
            height: Theme.iconSizeMedium
            radius: width / 2
            color: Theme.highlightColor
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

            property bool acceptedTouchIcon
            property int frameCounter
            property bool rendered
            // textSelectionActive and textSelectionController are FINAL in WPEWebPage C++
            property Item _selectionUI: null
            readonly property bool activeWebPage: container.tabId == tabId
            property bool userHasDraggedWhileLoading
            property string favicon

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

                        if (!webView.privateMode) {
                            // Update the favicon for history items.
                            FaviconManager.grabIcon("history", webPage,
                                                    Qt.size(Theme.iconSizeMedium,
                                                            Theme.iconSizeMedium))
                        }
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
                    favicon = ""
                    acceptedTouchIcon = false
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
                    if (acceptedTouchIcon)
                        return

                    acceptedTouchIcon = data.isRichIcon
                    favicon = data.url
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
            onRecvSyncMessage: {
                // sender expects that this handler will update `response` argument
                switch (message) {
                case "Content:SelectionCopied": {
                    if (data.succeeded && _selectionUI) {
                        _selectionUI.showNotification()
                        response.message = {"": ""}
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

                PermissionManager.instance()
            }
        }
    }
}
