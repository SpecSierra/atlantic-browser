/****************************************************************************
**
** Copyright (c) 2021 Open Mobile Platform LLC.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Browser 1.0
import "components"

Dialog {
    id: page

    property int uid: -1
    readonly property bool creating: uid < 0
    property LoginModel loginModel
    property SecureAction secureAction
    property alias hostname: hostnameField.text
    property alias username: usernameField.text
    property alias password: passwordField.text

    canAccept: creating
               ? (hostname.length > 0 && loginModel.canAdd(hostname, username))
               : loginModel.canModify(uid, username, password)
    onAcceptBlocked: usernameField.errorHighlight = true
    onAccepted: {
        if (creating)
            loginModel.add(hostname, username, password)
        else
            loginModel.modify(uid, username, password)
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column

            width: parent.width

            DialogHeader {
                title: page.creating
                       //% "Add login"
                       ? qsTrId("sailfish_browser-me-login_add_login")
                       //% "Edit login"
                       : qsTrId("sailfish_browser-me-login_edit_login")
            }

            TextField {
                id: hostnameField

                width: parent.width
                readOnly: !page.creating
                //% "Hostname"
                label: qsTrId("sailfish_browser-me-login_edit_hostname")
                //% "example.com"
                placeholderText: qsTrId("sailfish_browser-ph-login_hostname")
                inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase | Qt.ImhUrlCharactersOnly

                EnterKey.onClicked: usernameField.focus = true
            }

            TextField {
                id: usernameField

                //% "Username"
                label: qsTrId("sailfish_browser-me-login_edit_username")
                inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase

                EnterKey.onClicked: passwordField.focus = true

                onTextChanged: errorHighlight = false
                //% "This login already exists for the host"
                description: errorHighlight ? qsTrId("sailfish_browser-me-login_edit_login_collision") : ""
            }

            PasswordField {
                id: passwordField

                EnterKey.onClicked: focus = false
                showEchoModeToggle: secureAction.available

                _automaticEchoModeToggle: false

                on_EchoModeToggleClicked: {
                    if (_usePasswordEchoMode) {
                        secureAction.perform(function () { _usePasswordEchoMode = false })
                    } else {
                        _usePasswordEchoMode = true
                    }
                }
            }

            Item {
                width: parent.width
                height: Theme.paddingSmall
            }
        }

        VerticalScrollDecorator {}
    }
}
