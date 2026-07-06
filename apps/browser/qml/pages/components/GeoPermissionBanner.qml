/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import Sailfish.Silica 1.0

// Shown when a page asks for the user's location. Allow/deny is remembered
// per host for the session (see WPEWebPage::resolveGeoPermission). Driven
// entirely by the bound webView's contentItem.
Rectangle {
    id: geoPermissionBanner

    property var webView
    readonly property var page: webView ? webView.contentItem : null

    visible: page !== null && page.geoPermissionPending
    anchors {
        left: parent.left
        right: parent.right
        bottom: parent.bottom
        bottomMargin: page ? page.toolbarHeight : 0
    }
    height: geoColumn.height + Theme.paddingLarge * 2
    color: Theme.overlayBackgroundColor
    z: 900

    Column {
        id: geoColumn
        anchors {
            left: parent.left
            right: parent.right
            verticalCenter: parent.verticalCenter
            margins: Theme.paddingLarge
        }
        spacing: Theme.paddingMedium

        Image {
            source: "image://theme/icon-m-gps?" + Theme.highlightColor
            anchors.horizontalCenter: parent.horizontalCenter
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Theme.fontSizeMedium
            color: Theme.highlightColor
            wrapMode: Text.WordWrap
            text: qsTr("%1 wants to know your location").arg(page ? page.geoPermissionHost : "")
        }
        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("Allow")
            onClicked: if (geoPermissionBanner.page) geoPermissionBanner.page.resolveGeoPermission(true)
        }
        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("Deny")
            onClicked: if (geoPermissionBanner.page) geoPermissionBanner.page.resolveGeoPermission(false)
        }
    }
}
