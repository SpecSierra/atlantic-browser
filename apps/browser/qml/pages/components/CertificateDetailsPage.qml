/*
 * Copyright (c) 2024 Jolla Ltd.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

import QtQuick 2.6
import Sailfish.Silica 1.0
import Sailfish.Silica.Background 1.0 as SilicaBackground

Page {
    id: certDetailsPage

    SilicaBackground.Background {
        anchors.fill: parent
        z: -1
    }

    property var security

    readonly property bool _secure: security && security.allGood

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        VerticalScrollDecorator {}

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingMedium

            PageHeader {
                //% "Certificate Details"
                title: qsTrId("sailfish_browser-he-certificate_details")
            }

            // Status icon and text
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
                text: _secure
                      //% "Connection is secure"
                      ? qsTrId("sailfish_browser-la-cert_connection_secure")
                      //% "Connection is not secure"
                      : qsTrId("sailfish_browser-la-cert_connection_insecure")
            }

            // Error description (only for insecure)
            Label {
                width: parent.width - 2 * Theme.horizontalPageMargin
                x: Theme.horizontalPageMargin
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                color: Theme.errorColor
                visible: security && security.errorDescription && security.errorDescription.length > 0
                text: security ? security.errorDescription : ""
            }

            SectionHeader {
                //% "Subject"
                text: qsTrId("sailfish_browser-sh-cert_subject")
            }

            DetailItem {
                //% "Common Name"
                label: qsTrId("sailfish_browser-la-cert_cn")
                value: security ? security.subjectDisplayName : ""
                visible: value.length > 0
            }

            DetailItem {
                //% "Organization"
                label: qsTrId("sailfish_browser-la-cert_org")
                value: security ? security.subjectOrganization : ""
                visible: value.length > 0
            }

            SectionHeader {
                //% "Issuer"
                text: qsTrId("sailfish_browser-sh-cert_issuer")
            }

            DetailItem {
                //% "Common Name"
                label: qsTrId("sailfish_browser-la-cert_issuer_cn")
                value: security ? security.issuerCommonName : ""
                visible: value.length > 0
            }

            DetailItem {
                //% "Organization"
                label: qsTrId("sailfish_browser-la-cert_issuer_org")
                value: security ? security.issuerOrganization : ""
                visible: value.length > 0
            }

            DetailItem {
                //% "Full Name"
                label: qsTrId("sailfish_browser-la-cert_issuer_full")
                value: security ? security.issuerDisplayName : ""
                visible: value.length > 0
            }

            SectionHeader {
                //% "Validity"
                text: qsTrId("sailfish_browser-sh-cert_validity")
            }

            DetailItem {
                //% "Valid From"
                label: qsTrId("sailfish_browser-la-cert_valid_from")
                value: security ? security.notBefore : ""
                visible: value.length > 0
            }

            DetailItem {
                //% "Valid Until"
                label: qsTrId("sailfish_browser-la-cert_valid_until")
                value: security ? security.notAfter : ""
                visible: value.length > 0
            }

            SectionHeader {
                //% "Connection"
                text: qsTrId("sailfish_browser-sh-cert_connection")
            }

            DetailItem {
                //% "Cipher Suite"
                label: qsTrId("sailfish_browser-la-cert_cipher")
                value: security ? security.cipherName : ""
                visible: value.length > 0
            }

            // PEM certificate data
            SectionHeader {
                //% "Certificate (PEM)"
                text: qsTrId("sailfish_browser-sh-cert_pem")
                visible: pemLabel.visible
            }

            Label {
                id: pemLabel
                width: parent.width - 2 * Theme.horizontalPageMargin
                x: Theme.horizontalPageMargin
                wrapMode: Text.WrapAnywhere
                font.pixelSize: Theme.fontSizeTiny
                font.family: "monospace"
                color: Theme.secondaryColor
                visible: security && security.certificatePem && security.certificatePem.length > 0
                text: security ? security.certificatePem : ""
            }
        }
    }
}
