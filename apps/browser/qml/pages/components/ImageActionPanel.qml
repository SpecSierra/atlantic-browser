/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import Sailfish.Silica 1.0

// Image long-press panel. DockedPanel avoids the SilicaFlickable ancestor
// requirement that ContextMenu has, so it works when triggered programmatically
// from inside a plain Page. Opens itself in response to the bound webView's
// imageLongPressed signal and holds the pending image URL internally.
DockedPanel {
    id: imagePanel

    property var webView
    property string pendingImageUrl: ""

    width: parent.width
    height: Theme.itemSizeLarge + Theme.paddingLarge
    dock: Dock.Bottom
    open: false
    onOpenChanged: if (!open) pendingImageUrl = ""

    Row {
        anchors.centerIn: parent
        spacing: Theme.paddingLarge

        Button {
            //% "Save image"
            text: qsTrId("atlantic-bt-save_image")
            onClicked: {
                if (imagePanel.webView.contentItem && imagePanel.pendingImageUrl.length > 0)
                    imagePanel.webView.contentItem.downloadUrl(imagePanel.pendingImageUrl)
                imagePanel.open = false
            }
        }

        Button {
            //% "Cancel"
            text: qsTrId("atlantic-bt-cancel")
            onClicked: imagePanel.open = false
        }
    }

    Connections {
        target: imagePanel.webView
        onImageLongPressed: {
            if (imageUrl.length === 0) return
            imagePanel.pendingImageUrl = imageUrl
            imagePanel.open = true
        }
    }
}
