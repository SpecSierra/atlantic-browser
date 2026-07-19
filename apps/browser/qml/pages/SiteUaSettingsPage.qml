/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.6
import Sailfish.Silica 1.0
import Nemo.Configuration 1.0
import Sailfish.Silica.Background 1.0 as SilicaBackground

// Per-site user-agent overrides. Entries are stored in dconf as a JSON
// object { "host": "profile-id" }; BrowserPage.qml pushes the key into the
// engine (WPEWebPage::applySiteUaOverridesGlobally), where an entry for
// example.com also covers every subdomain. Profile ids must match
// uaForProfile() in WPEWebPage.cpp.
Page {
    id: page

    SilicaBackground.Background {
        anchors.fill: parent
        z: -1
    }

    readonly property var uaProfiles: [
        //% "Chrome on Android"
        { id: "chrome-android", label: qsTrId("atlantic-me-ua_chrome_android") },
        //% "Safari on iPhone"
        { id: "safari-iphone", label: qsTrId("atlantic-me-ua_safari_iphone") },
        //% "Firefox on Android"
        { id: "firefox-android", label: qsTrId("atlantic-me-ua_firefox_android") },
        //% "Chrome on Windows"
        { id: "chrome-desktop", label: qsTrId("atlantic-me-ua_chrome_desktop") },
        //% "Safari on Mac"
        { id: "safari-mac", label: qsTrId("atlantic-me-ua_safari_mac") },
        //% "Firefox on Linux"
        { id: "firefox-desktop", label: qsTrId("atlantic-me-ua_firefox_desktop") }
    ]

    function profileLabel(id) {
        for (var i = 0; i < uaProfiles.length; i++) {
            if (uaProfiles[i].id === id)
                return uaProfiles[i].label
        }
        return id
    }

    function readOverrides() {
        try {
            var parsed = JSON.parse(siteUaOverridesConf.value)
            return (parsed && typeof parsed === "object") ? parsed : {}
        } catch (e) {
            return {}
        }
    }

    function writeOverrides(overrides) {
        siteUaOverridesConf.value = JSON.stringify(overrides)
        refreshModel()
    }

    // Accepts a full URL or bare host; stores the bare lowercase host.
    function normalizeHost(input) {
        var host = input.trim().toLowerCase()
        host = host.replace(/^[a-z]+:\/\//, "")     // scheme
        host = host.split("/")[0].split("?")[0]     // path / query
        host = host.split(":")[0]                   // port
        if (host.indexOf("www.") === 0)
            host = host.substring(4)
        return host
    }

    function setOverride(host, profileId) {
        var overrides = readOverrides()
        overrides[host] = profileId
        writeOverrides(overrides)
    }

    function removeOverride(host) {
        var overrides = readOverrides()
        delete overrides[host]
        writeOverrides(overrides)
    }

    function refreshModel() {
        var overrides = readOverrides()
        var hosts = Object.keys(overrides).sort()
        siteModel.clear()
        for (var i = 0; i < hosts.length; i++)
            siteModel.append({ host: hosts[i], profileId: overrides[hosts[i]] })
    }

    ConfigurationValue {
        id: siteUaOverridesConf
        key: "/apps/atlantic-browser/settings/site_ua_overrides"
        defaultValue: "{}"
    }

    ListModel {
        id: siteModel
    }

    Component.onCompleted: refreshModel()

    SilicaListView {
        anchors.fill: parent
        model: siteModel

        header: PageHeader {
            //% "Site user agents"
            title: qsTrId("atlantic-he-site_user_agents")
            //% "Make selected websites see a different browser"
            description: qsTrId("atlantic-he-site_user_agents_description")
        }

        PullDownMenu {
            MenuItem {
                //% "Add website"
                text: qsTrId("atlantic-me-add_website")
                onClicked: {
                    var dialog = pageStack.push(addSiteDialog)
                    dialog.accepted.connect(function() {
                        var host = normalizeHost(dialog.host)
                        if (host.length > 0)
                            setOverride(host, dialog.profileId)
                    })
                }
            }
        }

        ViewPlaceholder {
            enabled: siteModel.count === 0
            //% "No websites added"
            text: qsTrId("atlantic-la-no_websites_added")
            //% "Pull down to make a website see a different browser, e.g. if it refuses to work in Atlantic"
            hintText: qsTrId("atlantic-la-no_websites_added_hint")
        }

        delegate: ListItem {
            id: siteItem
            width: parent.width
            contentHeight: Theme.itemSizeMedium

            function remove() {
                remorseDelete(function() { removeOverride(model.host) })
            }

            Column {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                anchors.verticalCenter: parent.verticalCenter

                Label {
                    width: parent.width
                    text: model.host
                    truncationMode: TruncationMode.Fade
                    color: siteItem.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
                Label {
                    width: parent.width
                    text: profileLabel(model.profileId)
                    truncationMode: TruncationMode.Fade
                    font.pixelSize: Theme.fontSizeSmall
                    color: siteItem.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                }
            }

            // ContextMenu delegates lose the ListView's model context, so
            // snapshot the row values on the item itself.
            readonly property string hostValue: model.host
            readonly property string profileValue: model.profileId

            menu: ContextMenu {
                Repeater {
                    model: page.uaProfiles
                    delegate: MenuItem {
                        text: modelData.label
                        font.bold: modelData.id === siteItem.profileValue
                        onClicked: setOverride(siteItem.hostValue, modelData.id)
                    }
                }
                MenuItem {
                    //% "Remove"
                    text: qsTrId("atlantic-me-remove_website")
                    onClicked: siteItem.remove()
                }
            }
        }

        VerticalScrollDecorator {}
    }

    Component {
        id: addSiteDialog

        Dialog {
            id: dialog

            property alias host: hostField.text
            property string profileId: page.uaProfiles[profileCombo.currentIndex].id

            canAccept: hostField.text.trim().length > 0

            SilicaBackground.Background {
                anchors.fill: parent
                z: -1
            }

            Column {
                width: parent.width

                DialogHeader {
                    //% "Add"
                    acceptText: qsTrId("atlantic-he-add_site_accept")
                }

                TextField {
                    id: hostField
                    width: parent.width
                    focus: true
                    //% "Website"
                    label: qsTrId("atlantic-la-website")
                    //% "e.g. maps.google.com"
                    placeholderText: qsTrId("atlantic-ph-website_example")
                    inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase | Qt.ImhUrlCharactersOnly
                    EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                    EnterKey.onClicked: dialog.accept()
                }

                ComboBox {
                    id: profileCombo
                    width: parent.width
                    //% "Appear as"
                    label: qsTrId("atlantic-la-appear_as")

                    menu: ContextMenu {
                        Repeater {
                            model: page.uaProfiles
                            delegate: MenuItem { text: modelData.label }
                        }
                    }
                }
            }
        }
    }
}
