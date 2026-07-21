/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import Sailfish.Silica 1.0

// "Save password?" prompt (Phase 3 capture). Raised when the loginBridge
// reports a submitted login that is new or changed vs the encrypted store.
// Driven entirely by the bound webView's contentItem — a plain property
// binding, not a signal handler (the script-message callback runs outside the
// QML JS context; see ImageActionPanel/PermissionBanner for the same reason).
Rectangle {
    id: saveLoginBanner

    property var webView
    readonly property var page: webView ? webView.contentItem : null

    readonly property string promptText: {
        if (!page)
            return ""
        var host = page.saveLoginHost
        if (page.saveLoginIsUpdate)
            //% "Update saved password for %1?"
            return qsTrId("sailfish_browser-la-update_password").arg(host)
        //% "Save password for %1?"
        return qsTrId("sailfish_browser-la-save_password").arg(host)
    }

    visible: page !== null && page.saveLoginPending
    anchors {
        left: parent.left
        right: parent.right
        bottom: parent.bottom
        bottomMargin: page ? page.toolbarHeight : 0
    }
    height: saveLoginColumn.height + Theme.paddingLarge * 2
    color: Theme.overlayBackgroundColor
    z: 900

    Column {
        id: saveLoginColumn
        anchors {
            left: parent.left
            right: parent.right
            verticalCenter: parent.verticalCenter
            margins: Theme.paddingLarge
        }
        spacing: Theme.paddingMedium

        Image {
            source: "image://theme/icon-m-keys?" + Theme.highlightColor
            anchors.horizontalCenter: parent.horizontalCenter
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Theme.fontSizeMedium
            color: Theme.highlightColor
            wrapMode: Text.WordWrap
            text: saveLoginBanner.promptText
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.secondaryHighlightColor
            truncationMode: TruncationMode.Fade
            visible: text.length > 0
            text: saveLoginBanner.page ? saveLoginBanner.page.saveLoginUsername : ""
        }
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: Theme.paddingLarge
            Button {
                //% "Save"
                text: qsTrId("sailfish_browser-bt-save_login")
                onClicked: if (saveLoginBanner.page) saveLoginBanner.page.resolveSaveLogin(true)
            }
            Button {
                //% "Not now"
                text: qsTrId("sailfish_browser-bt-not_now")
                onClicked: if (saveLoginBanner.page) saveLoginBanner.page.resolveSaveLogin(false)
            }
        }
    }
}
