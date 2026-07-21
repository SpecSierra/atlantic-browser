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
import Sailfish.Silica.Background 1.0 as SilicaBackground
import "components"

Page {
    SilicaBackground.Background {
        anchors.fill: parent
        z: -1
    }

    function stripPrefix(hostname) {
        if (hostname.length < 7) {
            return hostname
        }
        if (hostname.substring(0, 8) === "https://") {
            return hostname.substring(8)
        } else if (hostname.substring(0, 7) === "http://") {
            return hostname.substring(7)
        }
        return hostname
    }

    function copyUsername(username) {
        Clipboard.text = username
        //% "Username copied"
        notification.text = qsTrId("sailfish_browser-me-login_copied_username")
        notification.show()
    }

    // Should only be called with SecureAction to avoid leaking passwords
    function copyPassword(password) {
        Clipboard.text = password
        //% "Password copied"
        notification.text = qsTrId("sailfish_browser-me-login_copied_password")
        notification.show()
    }

    // Master-password gate: shown until the vault is unlocked (or created).
    // A single set of PasswordFields serves both flows; hasVault picks which.
    Column {
        id: gate

        readonly property bool showing: loginModel.locked

        visible: showing
        opacity: showing ? 1.0 : 0.0
        width: parent.width - 2 * Theme.horizontalPageMargin
        x: Theme.horizontalPageMargin
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.paddingLarge

        function submit() {
            gate.error = false
            if (loginModel.hasVault) {
                if (!loginModel.unlock(passwordField.text)) {
                    gate.error = true
                    passwordField.text = ""
                    passwordField.focus = true
                }
            } else {
                if (passwordField.text.length < 4) {
                    gate.error = true
                    return
                }
                if (passwordField.text !== confirmField.text) {
                    gate.mismatch = true
                    return
                }
                loginModel.createVault(passwordField.text)
            }
        }

        property bool error: false
        property bool mismatch: false

        PageHeader {
            //% "Passwords"
            title: qsTrId("sailfish_browser-he-passwords")
        }

        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            color: Theme.highlightColor
            text: loginModel.hasVault
                  //% "Enter your master password to unlock your saved passwords."
                  ? qsTrId("sailfish_browser-la-login_unlock_hint")
                  //% "Set a master password to protect your saved passwords. You'll need it to unlock them."
                  : qsTrId("sailfish_browser-la-login_setup_hint")
        }

        PasswordField {
            id: passwordField
            width: parent.width
            //% "Master password"
            label: qsTrId("sailfish_browser-la-login_master_password")
            errorHighlight: gate.error
            //% "Wrong password"
            description: gate.error && loginModel.hasVault
                         ? qsTrId("sailfish_browser-la-login_wrong_password") : ""
            EnterKey.iconSource: loginModel.hasVault ? "image://theme/icon-m-enter-accept" : "image://theme/icon-m-enter-next"
            EnterKey.onClicked: {
                if (loginModel.hasVault) gate.submit()
                else confirmField.focus = true
            }
            onTextChanged: { gate.error = false; gate.mismatch = false }
        }

        PasswordField {
            id: confirmField
            width: parent.width
            visible: !loginModel.hasVault
            //% "Confirm password"
            label: qsTrId("sailfish_browser-la-login_confirm_password")
            errorHighlight: gate.mismatch
            //% "Passwords don't match"
            description: gate.mismatch ? qsTrId("sailfish_browser-la-login_password_mismatch") : ""
            EnterKey.iconSource: "image://theme/icon-m-enter-accept"
            EnterKey.onClicked: gate.submit()
            onTextChanged: gate.mismatch = false
        }

        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            text: loginModel.hasVault
                  //% "Unlock"
                  ? qsTrId("sailfish_browser-bt-login_unlock")
                  //% "Create"
                  : qsTrId("sailfish_browser-bt-login_create")
            onClicked: gate.submit()
        }
    }

    SilicaListView {
        id: view

        anchors.fill: parent
        model: loginFilterModel
        currentIndex: -1
        visible: !gate.showing

        PullDownMenu {
            MenuItem {
                //% "Lock"
                text: qsTrId("sailfish_browser-me-login_lock")
                onClicked: loginModel.lock()
            }
            MenuItem {
                //% "Add login"
                text: qsTrId("sailfish_browser-me-login_add")
                onClicked: pageStack.animatorPush("EditLoginPage.qml", {
                                                      loginModel: loginModel,
                                                      secureAction: secureAction
                                                  })
            }
        }

        header: Column {
            width: parent.width
            PageHeader {
                //% "Passwords"
                title: qsTrId("sailfish_browser-he-passwords")
            }
            SearchField {
                width: parent.width
                //% "Search"
                placeholderText: qsTrId("sailfish_browser-ph-logins_search")
                EnterKey.onClicked: focus = false
                onTextChanged: loginFilterModel.search = text
                visible: loginModel.count > 0
            }
        }

        delegate: ListItem {
            id: listItem

            width: parent.width
            contentHeight: Theme.itemSizeMedium
            ListView.onAdd: AddAnimation { target: listItem }
            ListView.onRemove: animateRemoval()

            function remove(uid) {
                remorseDelete(function() {
                    loginModel.remove(uid)
                })
            }

            onClicked: openMenu()

            Row {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                spacing: Theme.paddingMedium
                anchors.verticalCenter: parent.verticalCenter

                FavoriteIcon {
                    id: loginsIcon

                    anchors.verticalCenter: parent.verticalCenter
                    icon: model.favicon

                    sourceSize.width: Theme.iconSizeMedium
                    sourceSize.height: Theme.iconSizeMedium
                    width: Theme.iconSizeMedium
                    height: Theme.iconSizeMedium
                }

                Column {
                    id: column

                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - parent.spacing - loginsIcon.width
                    Label {
                        width: parent.width
                        text: Theme.highlightText(stripPrefix(model.hostname),
                                                  loginFilterModel.search,
                                                  Theme.highlightColor)
                        textFormat: Text.StyledText
                    }
                    Label {
                        width: parent.width
                        text: model.username
                        font.pixelSize: Theme.fontSizeExtraSmall
                        color: Theme.secondaryColor
                    }
                }
            }

            menu: Component {
                ContextMenu {
                    id: contextMenu

                    MenuItem {
                        visible: !secureAction.available
                        //% "Copy username"
                        text: qsTrId("sailfish_browser-me-login_copy_username")
                        onClicked: copyUsername(model.username)
                    }
                    MenuItem {
                        visible: secureAction.available
                        //% "Copy"
                        text: qsTrId("sailfish_browser-me-login_copy")
                        onClicked: {
                            // Hide existing menu items
                            var content = contextMenu._contentColumn
                            for (var i = 0; i < content.children.length; i++) {
                                content.children[i].visible = false
                            }
                            // Block menu closing
                            contextMenu.closeOnActivation = false
                            // Reset highlight
                            contextMenu._setHighlightedItem(null)
                            // Add sub-menu menu items
                            copyOptions.createObject(contextMenu, {
                                                         menu: contextMenu,
                                                         username: model.username,
                                                         password: model.password
                                                     })
                        }
                    }
                    MenuItem {
                        //% "Edit"
                        text: qsTrId("sailfish_browser-me-login_edit")
                        onClicked: {
                            var page = pageStack.animatorPush("EditLoginPage.qml", {
                                                                  loginModel: loginModel,
                                                                  secureAction: secureAction,
                                                                  uid: model.uid,
                                                                  hostname: model.hostname,
                                                                  username: model.username,
                                                                  password: model.password
                                                              })
                        }
                    }
                    MenuItem {
                        //% "Delete"
                        text: qsTrId("sailfish_browser-me-login_delete")
                        onClicked: {
                            remove(model.uid)
                        }
                    }
                }
            }
        }

        PageBusyIndicator {
            running: !loginModel.populated
        }

        ViewPlaceholder {
            //% "Your saved logins and passwords show up here"
            text: qsTrId("sailfish_browser-la-logins-none")
            enabled: loginModel.count === 0 && loginModel.populated
        }

        VerticalScrollDecorator {
            parent: view
            flickable: view
        }
    }

    Notice {
        id: notification

        property bool published

        duration: Notice.Short
        verticalOffset: -Theme.itemSizeMedium
    }

    SecureAction {
        id: secureAction
        //% "Unlock access to browser passwords"
        message: qsTrId("sailfish_browser-me-login_unlock_password_access")
    }

    LoginFilterModel {
        id: loginFilterModel

        sourceModel: LoginModel {
            id: loginModel
        }
    }

    Component {
        id: copyOptions

        Item {
            id: _copyOptions

            property Item menu
            property string username
            property string password

            // Order of items is reversed
            MenuItem {
                visible: secureAction.available
                //% "Copy password"
                text: qsTrId("sailfish_browser-me-login_copy_password")
                parent: menu._contentColumn // context menu touch requires menu items are children of content area
                onClicked: {
                    secureAction.perform(copyPassword.bind(null, _copyOptions.password))
                    menu.close()
                }
            }
            MenuItem {
                // "Copy username" (defined above in contextMenu)
                text: qsTrId("sailfish_browser-me-login_copy_username")
                parent: menu._contentColumn // context menu touch requires menu items are children of content area
                onClicked: {
                    copyUsername(username)
                    menu.close()
                }
            }

            Connections {
                target: menu
                onClosed: _copyOptions.destroy()
            }
        }
    }
}
