/****************************************************************************
**
** Copyright (c) 2020 - 2021 Open Mobile Platform LLC.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.6
import Sailfish.Silica 1.0

Page {
    id: page

    property string title
    property string url

    readonly property int _allow: 1
    readonly property int _deny: 2
    readonly property int _prompt: 3

    ListModel {
        id: permissionTypesModel
    }

    Component.onCompleted: {
        permissionTypesModel.append({
            //% "Location"
            title: qsTrId("sailfish_browser-ti-location"),
            type: "geolocation",
            capability: _prompt
        })
        permissionTypesModel.append({
            //% "Popup"
            title: qsTrId("sailfish_browser-ti-popup"),
            type: "popup",
            capability: _deny
        })
        permissionTypesModel.append({
            //% "Cookies"
            title: qsTrId("sailfish_browser-ti-cookies"),
            type: "cookie",
            capability: _allow
        })
        permissionTypesModel.append({
            //% "Camera"
            title: qsTrId("sailfish_browser-ti-camera"),
            type: "camera",
            capability: _prompt
        })
        permissionTypesModel.append({
            //% "Microphone"
            title: qsTrId("sailfish_browser-ti-microphone"),
            type: "microphone",
            capability: _prompt
        })
    }

    SilicaListView {
        anchors.fill: parent
        header: PageHeader {
            title: page.title
            description: page.url
        }

        model: permissionTypesModel

        delegate: ComboBox {
            label: model.title

            currentIndex: {
                switch(model.capability) {
                case _allow:
                    return 0
                case _deny:
                    return 1
                default:
                    return 2
                }
            }

            menu: ContextMenu {
                MenuItem {
                    //: Shown for context menu allow permission
                    //% "Allow"
                    text: qsTrId("sailfish_browser-me-allow")
                    onClicked: permissionTypesModel.setProperty(index, "capability", _allow)
                }
                MenuItem {
                    //: Shown for context menu block permission
                    //% "Block"
                    text: qsTrId("sailfish_browser-me-block")
                    onClicked: permissionTypesModel.setProperty(index, "capability", _deny)
                }
                MenuItem {
                    //: Shown for context menu always ask permission
                    //% "Always ask"
                    text: qsTrId("sailfish_browser-me-always_ask")
                    visible: model.type !== "popup" && model.type !== "cookie"
                    onClicked: permissionTypesModel.setProperty(index, "capability", _prompt)
                }
            }
        }
    }
}
