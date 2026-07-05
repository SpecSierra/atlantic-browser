/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import Sailfish.Silica 1.0

// Shown when a page load is blocked by a TLS certificate failure (self-signed,
// expired, wrong host, ...). Offers to accept the certificate for this host and
// retry. Dismissed automatically when a new load starts (tlsErrorPending resets).
// Driven entirely by the bound webView's contentItem.
Rectangle {
    id: tlsErrorBanner

    property var webView
    readonly property var page: webView ? webView.contentItem : null

    // Local dismiss: hides the banner without accepting; resets on the next
    // TLS failure (tlsErrorPending toggles via a new load) or tab switch.
    property bool dismissed

    onPageChanged: dismissed = false
    Connections {
        target: tlsErrorBanner.page
        onTlsErrorChanged: tlsErrorBanner.dismissed = false
    }

    visible: page !== null && page.tlsErrorPending && !dismissed
    anchors {
        left: parent.left
        right: parent.right
        bottom: parent.bottom
        bottomMargin: page ? page.toolbarHeight : 0
    }
    height: tlsColumn.height + Theme.paddingLarge * 2
    color: Theme.overlayBackgroundColor
    z: 900

    Column {
        id: tlsColumn
        anchors {
            left: parent.left
            right: parent.right
            verticalCenter: parent.verticalCenter
            margins: Theme.paddingLarge
        }
        spacing: Theme.paddingMedium

        Image {
            source: "image://theme/icon-m-warning?" + Theme.errorColor
            anchors.horizontalCenter: parent.horizontalCenter
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Theme.fontSizeMedium
            color: Theme.errorColor
            wrapMode: Text.WordWrap
            text: qsTr("Connection to %1 is not secure").arg(page ? page.tlsErrorHost : "")
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.secondaryColor
            wrapMode: Text.WordWrap
            text: page && page.tlsErrorMessage.length > 0
                  ? qsTr("The site's security certificate is not trusted: %1.").arg(page.tlsErrorMessage)
                  : qsTr("The site's security certificate is not trusted.")
        }
        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("Accept and continue anyway")
            onClicked: if (tlsErrorBanner.page) tlsErrorBanner.page.acceptTlsCertificate()
        }
        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("Dismiss")
            onClicked: tlsErrorBanner.dismissed = true
        }
    }
}
