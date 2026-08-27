/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.6
import Sailfish.Silica 1.0
import Sailfish.Browser 1.0
import Sailfish.Pickers 1.0
import Sailfish.Silica.Background 1.0 as SilicaBackground

// Installed WebExtensions. The model is WebExtensionManager itself (C++), which
// also owns install/uninstall and the enabled-state registry — the page never
// touches the extensions directory directly.
Page {
    id: page

    // Popup and options pages open as ordinary tabs, so leaving settings is
    // part of the action: pop all the way back to the browser rather than
    // dropping the user on the settings page they came from.
    function returnToBrowser() {
        var browser = pageStack.find(function(candidate) {
            return candidate.objectName === "atlanticBrowserPage"
        })
        if (browser)
            pageStack.pop(browser)
        else
            pageStack.pop()
    }

    SilicaBackground.Background {
        anchors.fill: parent
        z: -1
    }

    SilicaListView {
        id: listView

        anchors.fill: parent
        model: WebExtensionManager

        PullDownMenu {
            MenuItem {
                //% "Reload extensions"
                text: qsTrId("atlantic-me-extensions_reload")
                onClicked: WebExtensionManager.reload()
            }
            MenuItem {
                //% "Get extensions"
                text: qsTrId("atlantic-me-extensions_browse")
                onClicked: pageStack.push("ExtensionStorePage.qml")
            }
            MenuItem {
                //% "Install from file…"
                text: qsTrId("atlantic-me-extensions_install")
                onClicked: pageStack.animatorPush(filePickerPage)
            }
        }

        header: Column {
            width: page.width

            PageHeader {
                //% "Extensions"
                title: qsTrId("atlantic-he-extensions")
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                visible: listView.count === 0
                //% "No extensions installed. Pull down to browse addons.mozilla.org, install a .zip or .xpi package, or copy an unpacked extension into %1."
                text: qsTrId("atlantic-la-extensions_empty").arg(WebExtensionManager.extensionsDirectory)
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.errorColor
                visible: WebExtensionManager.lastError !== ""
                text: WebExtensionManager.lastError
            }
        }

        delegate: ListItem {
            id: item

            width: page.width
            contentHeight: content.height + 2 * Theme.paddingMedium

            menu: ContextMenu {
                MenuItem {
                    //% "Open popup"
                    text: qsTrId("atlantic-me-extension_open_popup")
                    visible: model.popupUrl !== ""
                    onClicked: {
                        WebExtensionManager.openActionPopup(model.extensionId)
                        page.returnToBrowser()
                    }
                }
                MenuItem {
                    //% "Options"
                    text: qsTrId("atlantic-me-extension_options")
                    visible: model.optionsUrl !== ""
                    onClicked: {
                        WebExtensionManager.openOptionsPage(model.extensionId)
                        page.returnToBrowser()
                    }
                }
                MenuItem {
                    text: model.enabled
                          //% "Disable"
                          ? qsTrId("atlantic-me-extension_disable")
                          //% "Enable"
                          : qsTrId("atlantic-me-extension_enable")
                    onClicked: WebExtensionManager.setExtensionEnabled(model.extensionId, !model.enabled)
                }
                MenuItem {
                    //% "Remove"
                    text: qsTrId("atlantic-me-extension_remove")
                    onClicked: item.remorseAction(
                        //% "Removing %1"
                        qsTrId("atlantic-la-extension_removing").arg(model.name),
                        function() { WebExtensionManager.uninstall(model.extensionId) })
                }
            }

            Column {
                id: content

                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                y: Theme.paddingMedium
                spacing: Theme.paddingSmall

                Row {
                    width: parent.width
                    spacing: Theme.paddingMedium

                    Item {
                        id: icon

                        width: Theme.iconSizeMedium
                        height: Theme.iconSizeMedium
                        opacity: model.enabled ? 1.0 : 0.4
                        anchors.verticalCenter: parent.verticalCenter

                        // The extension's own icon is full colour and must not
                        // be tinted; the fallback glyph is monochrome and must
                        // be. Hence two elements rather than one switched source.
                        Image {
                            anchors.fill: parent
                            sourceSize.width: width
                            sourceSize.height: height
                            fillMode: Image.PreserveAspectFit
                            visible: model.iconPath !== ""
                            source: model.iconPath
                        }

                        Icon {
                            anchors.fill: parent
                            sourceSize.width: width
                            sourceSize.height: height
                            visible: model.iconPath === ""
                            source: Qt.resolvedUrl("../icons/icon-m-extension.svg")
                        }
                    }

                    Column {
                        width: parent.width - icon.width - parent.spacing
                        anchors.verticalCenter: parent.verticalCenter

                        Label {
                            width: parent.width
                            truncationMode: TruncationMode.Fade
                            text: model.name
                            color: model.enabled
                                   ? (item.highlighted ? Theme.highlightColor : Theme.primaryColor)
                                   : Theme.secondaryColor
                        }
                        Label {
                            width: parent.width
                            truncationMode: TruncationMode.Fade
                            font.pixelSize: Theme.fontSizeExtraSmall
                            color: item.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                            text: model.enabled
                                  ? model.version
                                    //% "%1 — disabled"
                                  : qsTrId("atlantic-la-extension_disabled").arg(model.version)
                        }
                    }
                }

                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                    visible: model.description !== ""
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: item.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                    text: model.description
                }

                // Anything the extension asked for that Atlantic will not do.
                // Shown per extension rather than in a single generic notice, so
                // it is obvious which one is degraded and how.
                Repeater {
                    model: modelData_warnings
                    Label {
                        width: content.width
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.fontSizeExtraSmall
                        color: Theme.errorColor
                        text: "⚠ " + modelData
                    }
                }

                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    visible: model.hostPermissions.length > 0
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: item.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                    //% "Site access: %1"
                    text: qsTrId("atlantic-la-extension_site_access").arg(model.hostPermissions.join(", "))
                }
            }

            readonly property var modelData_warnings: model.warnings

            onClicked: WebExtensionManager.setExtensionEnabled(model.extensionId, !model.enabled)
        }

        VerticalScrollDecorator {}
    }

    Component {
        id: filePickerPage

        FilePickerPage {
            //% "Select an extension package"
            title: qsTrId("atlantic-ti-extension_pick")
            nameFilters: ["*.zip", "*.xpi", "*.crx"]

            onSelectedContentPropertiesChanged: {
                WebExtensionManager.install(selectedContentProperties.filePath)
            }
        }
    }
}
