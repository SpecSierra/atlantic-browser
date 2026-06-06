/*
 * Copyright (c) 2019 Open Mobile Platform LLC.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

import QtQuick 2.6
import Sailfish.Silica 1.0
import Sailfish.Silica.private 1.0 as Private
import Sailfish.Silica.Background 1.0 as SilicaBackground
import Sailfish.Browser 1.0
import "." as Browser
import "../../shared" as Shared

SilicaFlickable {
    id: root

    property var security
    readonly property bool _validCert: security && security.subjectDisplayName.length > 0
    readonly property bool _secure: security && security.allGood

    contentHeight: certInfoColumn.height
    clip: contentHeight > height

    signal showCertDetail
    signal closeCertInfo

    onSecurityChanged: {
        // Jump back to the top
        contentY = originY
    }

    Shared.Background {
        anchors.fill: parent
        z: -2
    }

    VerticalScrollDecorator {}

    MouseArea {
        anchors.fill: parent
        onClicked: closeCertInfo()
    }

    Column {
        id: certInfoColumn

        width: parent.width
        spacing: Theme.paddingMedium
        topPadding: Theme.paddingLarge
        bottomPadding: Theme.paddingLarge

        Image {
            source: _secure ? "image://theme/icon-m-device-lock" : "image://theme/icon-m-warning"
            anchors.horizontalCenter: parent.horizontalCenter
        }
        Label {
            width: parent.width - 2 * Theme.horizontalPageMargin
            x: Theme.horizontalPageMargin
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSizeLarge
            color: _secure ? Theme.primaryColor : Theme.errorColor
            text: {
                if (_secure) {
                    //: The SSL/TLS connection is good
                    //% "Connection is secure"
                    return qsTrId("sailfish_browser-la-cert_connection_secure")
                }
                //: Either no SSL/TLS is in use, or the SSL/TLS connection is broken in some way
                //% "Connection is not secure"
                return qsTrId("sailfish_browser-la-cert_connection_insecure")
            }
        }

        DetailItem {
            //: Label for the issuer field of a TLS certificate
            //% "Verified by"
            label: qsTrId("sailfish_browser-la-cert_issuer")
            value: security ? security.issuerDisplayName : ""
            visible: value && value.length > 0
        }

        DetailItem {
            //: Label for the cipher suite field of a TLS certificate
            //% "Cipher suite"
            label: qsTrId("sailfish_browser-la-cipher_suite")
            value: security ? security.cipherName : ""
            visible: value && value.length > 0
        }

        Label {
            width: parent.width - 2 * Theme.horizontalPageMargin
            x: Theme.horizontalPageMargin
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: Theme.secondaryHighlightColor
            visible: !_secure
            //% "Do not enter personal data, passwords, card details on this site"
            text: qsTrId("sailfish_browser-sh-do_not-enter_personal_data")
        }

        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            //: Details for certificate info
            //% "Details"
            text: qsTrId("sailfish_browser-sh-details")
            enabled: security && !security.certIsNull
            visible: enabled
            onClicked: showCertDetail()
        }
    }
}
