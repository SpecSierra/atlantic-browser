/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import Sailfish.Silica 1.0
import Sailfish.Browser 1.0

// Long-press action panel. Originally image-only, now also the surface for
// links and for browser.contextMenus entries — Atlantic has no menu bar, so a
// long press is where all of those have to live.
//
// Driven by the bound webView's contentItem PROPERTIES (imageLongPressUrl and
// contextInfo) — NOT a recvAsyncMessage/signal handler, which never fires
// because the WebKit script-message callback runs outside the QML JS context
// (same reason SelectMenuOverlay uses selectMenuActive bindings). DockedPanel
// avoids the SilicaFlickable ancestor requirement that ContextMenu has, so it
// works when triggered from inside a plain Page.
DockedPanel {
    id: imagePanel

    property var webView
    readonly property string pendingImageUrl:
        (webView && webView.contentItem) ? webView.contentItem.imageLongPressUrl : ""
    readonly property var context:
        (webView && webView.contentItem) ? webView.contentItem.contextInfo : ({})
    // Set for a long press that landed anywhere inside an <a href>, image links
    // included — those offer both the link and the image actions. Restricted to
    // http(s) because that is all "open in new tab" can do anything with; keep
    // in sync with isNavigableLink() in WPEUserScripts.h, which decides whether
    // the press is worth reporting at all.
    readonly property string pendingLinkUrl: {
        var url = (context && context.linkUrl) ? context.linkUrl : ""
        return /^https?:\/\//i.test(url) ? url : ""
    }

    // Re-queried whenever the long press reports a new context, or an extension
    // adds or removes an item.
    property var extensionItems: []

    function refreshExtensionItems() {
        extensionItems = (context && context.pageUrl)
                ? WebExtensionManager.contextMenuItems(context)
                : []
    }

    function clear() {
        if (webView && webView.contentItem) {
            webView.contentItem.clearImageLongPress()
            webView.contentItem.clearContextInfo()
        }
    }

    onContextChanged: refreshExtensionItems()

    Connections {
        target: WebExtensionManager
        onContextMenuItemsChanged: imagePanel.refreshExtensionItems()
    }

    width: parent.width
    // Every row is optional, so the height follows the content rather than
    // assuming the built-in row is always there.
    height: panelContent.height + 2 * Theme.paddingMedium
    dock: Dock.Bottom
    // Pure binding: opens for a long-pressed image or link, or when an
    // extension has something to offer for whatever was pressed. Closes when
    // they are all gone.
    open: pendingImageUrl.length > 0 || pendingLinkUrl.length > 0 || extensionItems.length > 0

    // Opaque black backdrop so the buttons aren't drawn over the live page.
    Rectangle {
        anchors.fill: parent
        color: "black"
    }

    Column {
        id: panelContent

        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            topMargin: Theme.paddingMedium
        }
        spacing: Theme.paddingMedium

        Column {
            id: actionColumn

            width: parent.width

            // Built-in actions and extension entries share one list so the
            // panel stays a single column whatever the press landed on —
            // buttons in a row ran out of width once links were added.
            BackgroundItem {
                width: actionColumn.width
                height: Theme.itemSizeSmall
                visible: imagePanel.pendingLinkUrl.length > 0

                Label {
                    anchors {
                        left: parent.left
                        right: parent.right
                        leftMargin: Theme.horizontalPageMargin
                        rightMargin: Theme.horizontalPageMargin
                        verticalCenter: parent.verticalCenter
                    }
                    truncationMode: TruncationMode.Fade
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                    //% "Open in new tab"
                    text: qsTrId("sailfish_browser-me-open_new_tab")
                }

                onClicked: {
                    var url = imagePanel.pendingLinkUrl
                    imagePanel.clear()
                    if (imagePanel.webView && url.length > 0) {
                        imagePanel.webView.clearSelection()
                        imagePanel.webView.load(url, "", true /* newTab */)
                    }
                }
            }

            BackgroundItem {
                width: actionColumn.width
                height: Theme.itemSizeSmall
                visible: imagePanel.pendingImageUrl.length > 0

                Label {
                    anchors {
                        left: parent.left
                        right: parent.right
                        leftMargin: Theme.horizontalPageMargin
                        rightMargin: Theme.horizontalPageMargin
                        verticalCenter: parent.verticalCenter
                    }
                    truncationMode: TruncationMode.Fade
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                    //% "Save image"
                    text: qsTrId("atlantic-bt-save_image")
                }

                onClicked: {
                    if (imagePanel.webView.contentItem && imagePanel.pendingImageUrl.length > 0)
                        imagePanel.webView.contentItem.downloadUrl(imagePanel.pendingImageUrl)
                    imagePanel.clear()
                }
            }

            Repeater {
                model: imagePanel.extensionItems

                BackgroundItem {
                    width: actionColumn.width
                    height: Theme.itemSizeSmall
                    enabled: modelData.enabled

                    Label {
                        anchors {
                            left: parent.left
                            right: parent.right
                            leftMargin: Theme.horizontalPageMargin
                            rightMargin: Theme.horizontalPageMargin
                            verticalCenter: parent.verticalCenter
                        }
                        truncationMode: TruncationMode.Fade
                        opacity: modelData.enabled ? 1.0 : Theme.opacityLow
                        color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                        text: modelData.type === "checkbox" && modelData.checked
                              ? "✓ " + modelData.title
                              : modelData.title
                    }

                    onClicked: {
                        WebExtensionManager.activateContextMenuItem(
                                    modelData.extensionId, modelData.itemId, imagePanel.context)
                        imagePanel.clear()
                    }
                }
            }
        }

        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            //% "Cancel"
            text: qsTrId("atlantic-bt-cancel")
            onClicked: imagePanel.clear()
        }
    }
}
