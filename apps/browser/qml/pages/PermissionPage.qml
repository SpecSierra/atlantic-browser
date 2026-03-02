/****************************************************************************
**
** Copyright (c) 2020 - 2021 Open Mobile Platform LLC.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.0
import Sailfish.Silica 1.0
import "components"

Page {
    // WPE: permission constants (replacing PermissionManager enum from Gecko)
    readonly property int _permAllow: 1
    readonly property int _permDeny: 2
    readonly property int _permPrompt: 0

    ListModel {
        id: permissionTypesModel
    }

    function initPermissionTypesModel() {
        permissionTypesModel.append({
            title: qsTrId("sailfish_browser-ti-geolocation"),
            type: "geolocation",
            capability: _permPrompt,
            iconSource: "image://theme/icon-m-browser-location",
            sensitiveData: true
        })
        permissionTypesModel.append({
            title: qsTrId("sailfish_browser-ti-popup"),
            type: "popup",
            capability: _permAllow,
            iconSource: "image://theme/icon-m-browser-popup",
            sensitiveData: false
        })
        permissionTypesModel.append({
            title: qsTrId("sailfish_browser-ti-cookies"),
            type: "cookie",
            capability: _permAllow,
            iconSource: "image://theme/icon-m-browser-cookies",
            sensitiveData: false
        })
        permissionTypesModel.append({
            title: qsTrId("sailfish_browser-ti-camera"),
            type: "camera",
            capability: _permPrompt,
            iconSource: "image://theme/icon-m-browser-camera",
            sensitiveData: true
        })
        permissionTypesModel.append({
            title: qsTrId("sailfish_browser-ti-microphone"),
            type: "microphone",
            capability: _permPrompt,
            iconSource: "image://theme/icon-m-browser-microphone",
            sensitiveData: true
        })
    }

    Component.onCompleted: initPermissionTypesModel()

    SilicaListView {
        anchors.fill: parent
        header: PageHeader {
            //% "Permissions"
            title: qsTrId("sailfish_browser-ti-permissions")
        }
        model: permissionTypesModel

        delegate: BrowserListItem {
            label: model.title
            value: {
                switch (model.capability) {
                case _permAllow:
                    //% "Allow"
                    return qsTrId("sailfish_browser-me-allow")
                case _permDeny:
                    //% "Block"
                    return qsTrId("sailfish_browser-me-block")
                default:
                    //% "Ask"
                    return qsTrId("sailfish_browser-me-ask")
                }
            }
            iconSource: model.iconSource

            menu: ContextMenu {
                MenuItem {
                    //% "Allow"
                    text: qsTrId("sailfish_browser-me-allow")
                    visible: !model.sensitiveData
                    onClicked: model.capability = _permAllow
                }
                MenuItem {
                    //% "Block"
                    text: qsTrId("sailfish_browser-me-block")
                    visible: !model.sensitiveData
                    onClicked: model.capability = _permDeny
                }
                MenuItem {
                    //% "Ask"
                    text: qsTrId("sailfish_browser-me-ask")
                    visible: model.sensitiveData
                }
            }
        }
    }
}
