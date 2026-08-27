/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import Sailfish.Silica 1.0
import Sailfish.Browser 1.0

// Long-press action panel. Originally image-only, now also the surface for
// browser.contextMenus entries — Atlantic has no menu bar, so a long press is
// where an extension's items have to live.
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
    // Grows with the extension entries; the built-in row is always present.
    height: Theme.itemSizeLarge + Theme.paddingLarge
            + extensionColumn.height + (extensionItems.length > 0 ? Theme.paddingMedium : 0)
    dock: Dock.Bottom
    // Pure binding: opens for a long-pressed image, or when an extension has
    // something to offer for whatever was pressed. Closes when both are gone.
    open: pendingImageUrl.length > 0 || extensionItems.length > 0

    // Opaque black backdrop so the buttons aren't drawn over the live page.
    Rectangle {
        anchors.fill: parent
        color: "black"
    }

    Column {
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            topMargin: Theme.paddingMedium
        }
        spacing: Theme.paddingMedium

        Column {
            id: extensionColumn

            width: parent.width

            Repeater {
                model: imagePanel.extensionItems

                BackgroundItem {
                    width: extensionColumn.width
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

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: Theme.paddingLarge

            Button {
                //% "Save image"
                text: qsTrId("atlantic-bt-save_image")
                visible: imagePanel.pendingImageUrl.length > 0
                onClicked: {
                    if (imagePanel.webView.contentItem && imagePanel.pendingImageUrl.length > 0)
                        imagePanel.webView.contentItem.downloadUrl(imagePanel.pendingImageUrl)
                    imagePanel.clear()
                }
            }

            Button {
                //% "Cancel"
                text: qsTrId("atlantic-bt-cancel")
                onClicked: imagePanel.clear()
            }
        }
    }
}
