/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import Sailfish.Silica 1.0

// Sits above the (now blank) web content after a WebProcess crash and offers a
// reload. Dismissed automatically when a new load starts (crashed resets to
// false). Driven entirely by the bound webView's contentItem.
Rectangle {
    id: crashBanner

    property var webView

    visible: webView && webView.contentItem !== null && webView.contentItem.crashed
    anchors {
        left: parent.left
        right: parent.right
        bottom: parent.bottom
        bottomMargin: webView && webView.contentItem ? webView.contentItem.toolbarHeight : 0
    }
    height: crashColumn.height + Theme.paddingLarge * 2
    color: Theme.overlayBackgroundColor
    z: 900

    Column {
        id: crashColumn
        anchors {
            left: parent.left
            right: parent.right
            verticalCenter: parent.verticalCenter
            margins: Theme.paddingLarge
        }
        spacing: Theme.paddingMedium

        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Theme.fontSizeMedium
            color: Theme.primaryColor
            text: qsTr("This tab has crashed")
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.secondaryColor
            wrapMode: Text.WordWrap
            text: qsTr("The page stopped unexpectedly. Your other tabs are fine.")
        }
        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("Reload")
            onClicked: crashBanner.webView.reload()
        }
    }
}
