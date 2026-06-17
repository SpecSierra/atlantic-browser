/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import Sailfish.Silica 1.0

// Image long-press panel. Driven by the bound webView's contentItem
// imageLongPressUrl PROPERTY binding — NOT a recvAsyncMessage/signal handler,
// which never fires because the WebKit script-message callback runs outside the
// QML JS context (same reason SelectMenuOverlay uses selectMenuActive bindings).
// DockedPanel avoids the SilicaFlickable ancestor requirement that ContextMenu
// has, so it works when triggered from inside a plain Page.
DockedPanel {
    id: imagePanel

    property var webView
    readonly property string pendingImageUrl:
        (webView && webView.contentItem) ? webView.contentItem.imageLongPressUrl : ""

    function clear() {
        if (webView && webView.contentItem)
            webView.contentItem.clearImageLongPress()
    }

    width: parent.width
    height: Theme.itemSizeLarge + Theme.paddingLarge
    dock: Dock.Bottom
    // Pure binding: opens when the active page reports a long-pressed image,
    // closes when the URL is cleared (Save/Cancel call clearImageLongPress()).
    open: pendingImageUrl.length > 0

    // Opaque black backdrop so the buttons aren't drawn over the live page.
    Rectangle {
        anchors.fill: parent
        color: "black"
    }

    Row {
        anchors.centerIn: parent
        spacing: Theme.paddingLarge

        Button {
            //% "Save image"
            text: qsTrId("atlantic-bt-save_image")
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
