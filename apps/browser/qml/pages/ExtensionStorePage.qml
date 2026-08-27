/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.6
import Sailfish.Silica 1.0
import Sailfish.Browser 1.0
import Sailfish.Silica.Background 1.0 as SilicaBackground

// Browse and install extensions. Empty search shows the curated catalog that
// ships with Atlantic; typing searches all of addons.mozilla.org. The catalog
// is advice, not a gate — anything found is installable, including add-ons
// whose verdict says they will not work here.
Page {
    id: page

    // The SearchField lives inside the list header, which is its own component:
    // its id is not visible out here, so it publishes itself on creation.
    property Item searchFieldItem
    property string pendingQuery

    function returnToBrowser() {
        var browser = pageStack.find(function(candidate) {
            return candidate.objectName === "atlanticBrowserPage"
        })
        if (browser)
            pageStack.pop(browser)
        else
            pageStack.pop()
    }

    function clearSearch() {
        if (searchFieldItem)
            searchFieldItem.text = ""
        WebExtensionStore.showCatalog()
    }

    function verdictColor(verdict) {
        switch (verdict) {
        case "works": return Theme.highlightColor
        case "partial": return Theme.secondaryHighlightColor
        case "broken": return Theme.errorColor
        default: return Theme.secondaryColor
        }
    }

    function verdictLabel(verdict) {
        switch (verdict) {
        //% "Works"
        case "works": return qsTrId("atlantic-la-extension_verdict_works")
        //% "Partly works"
        case "partial": return qsTrId("atlantic-la-extension_verdict_partial")
        //% "Will not work"
        case "broken": return qsTrId("atlantic-la-extension_verdict_broken")
        //% "Unknown"
        default: return qsTrId("atlantic-la-extension_verdict_unknown")
        }
    }

    // Why a verdict is what it is, in the user's terms rather than API names.
    function verdictDetail(verdict, reasons) {
        if (!reasons || reasons.length === 0)
            return ""
        if (verdict === "broken")
            //% "Needs %1, which Atlantic does not provide."
            return qsTrId("atlantic-la-extension_needs").arg(reasons.join(", "))
        //% "Some features need %1, which Atlantic does not provide."
        return qsTrId("atlantic-la-extension_some_features_need").arg(reasons.join(", "))
    }

    SilicaBackground.Background {
        anchors.fill: parent
        z: -1
    }

    SilicaListView {
        id: listView

        anchors.fill: parent
        model: WebExtensionStore
        currentIndex: -1

        PullDownMenu {
            busy: WebExtensionStore.busy

            MenuItem {
                //% "Install from a link…"
                text: qsTrId("atlantic-me-extension_install_link")
                onClicked: pageStack.animatorPush(linkDialog)
            }
            MenuItem {
                //% "Show the recommended list"
                text: qsTrId("atlantic-me-extension_show_catalog")
                visible: WebExtensionStore.searching
                onClicked: page.clearSearch()
            }
        }

        header: Column {
            width: page.width

            PageHeader {
                //% "Get extensions"
                title: qsTrId("atlantic-he-extension_store")
                //% "from addons.mozilla.org"
                description: qsTrId("atlantic-la-extension_store_source")
            }

            SearchField {
                id: searchField

                width: parent.width
                //% "Search addons.mozilla.org"
                placeholderText: qsTrId("atlantic-la-extension_search")
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText

                Component.onCompleted: page.searchFieldItem = searchField

                // Debounced: one request per pause in typing, not per keystroke.
                onTextChanged: {
                    page.pendingQuery = text
                    searchTimer.restart()
                }
                EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                EnterKey.onClicked: {
                    searchTimer.stop()
                    WebExtensionStore.search(text)
                    focus = false
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                visible: !WebExtensionStore.searching
                //% "These are known to suit Atlantic. Search to install anything else — Atlantic will not stop you, but it will tell you first what is not going to work."
                text: qsTrId("atlantic-la-extension_store_hint")
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                visible: WebExtensionStore.status !== ""
                text: WebExtensionStore.status
            }

            Item {
                width: 1
                height: Theme.paddingMedium
            }
        }

        delegate: ListItem {
            id: item

            width: page.width
            contentHeight: content.height + 2 * Theme.paddingMedium

            menu: ContextMenu {
                MenuItem {
                    text: model.verdict === "broken"
                          //% "Install anyway"
                          ? qsTrId("atlantic-me-extension_install_anyway")
                          //% "Install"
                          : qsTrId("atlantic-me-extension_install_one")
                    enabled: !model.installed && !model.installing
                    onClicked: WebExtensionStore.install(model.slug)
                }
                MenuItem {
                    //% "Open its page"
                    text: qsTrId("atlantic-me-extension_open_page")
                    visible: model.homepage !== ""
                    onClicked: {
                        WebExtensionStore.cancel()
                        WebExtensionManager.openUrl(model.homepage)
                        page.returnToBrowser()
                    }
                }
            }

            Column {
                id: content

                x: Theme.horizontalPageMargin
                y: Theme.paddingMedium
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingSmall

                Row {
                    width: parent.width
                    spacing: Theme.paddingMedium

                    Image {
                        id: icon

                        width: Theme.iconSizeMedium
                        height: Theme.iconSizeMedium
                        sourceSize.width: Theme.iconSizeMedium
                        sourceSize.height: Theme.iconSizeMedium
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        source: model.iconUrl !== "" ? model.iconUrl : "image://theme/icon-m-extension"
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    Column {
                        width: parent.width - icon.width - parent.spacing
                        anchors.verticalCenter: parent.verticalCenter

                        Label {
                            width: parent.width
                            truncationMode: TruncationMode.Fade
                            text: model.name
                            color: item.highlighted ? Theme.highlightColor : Theme.primaryColor
                        }

                        Row {
                            width: parent.width
                            spacing: Theme.paddingSmall

                            Label {
                                font.pixelSize: Theme.fontSizeExtraSmall
                                color: page.verdictColor(model.verdict)
                                text: page.verdictLabel(model.verdict)
                            }
                            Label {
                                font.pixelSize: Theme.fontSizeExtraSmall
                                color: Theme.secondaryColor
                                visible: model.curated && !model.verified
                                //% "· expected, not yet tested"
                                text: qsTrId("atlantic-la-extension_untested")
                            }
                            Label {
                                font.pixelSize: Theme.fontSizeExtraSmall
                                color: Theme.secondaryColor
                                visible: model.installed
                                //% "· installed"
                                text: qsTrId("atlantic-la-extension_installed")
                            }
                        }
                    }
                }

                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                    visible: model.summary !== ""
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: item.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                    text: model.summary
                }

                // The hand-written catalog note when there is one; otherwise the
                // verdict derived from the add-on's declared permissions.
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: model.verdict === "broken" ? Theme.errorColor : Theme.secondaryColor
                    visible: text !== ""
                    text: model.note !== ""
                          ? model.note
                          : page.verdictDetail(model.verdict, model.verdictReasons)
                }
            }

            ProgressBar {
                anchors.bottom: parent.bottom
                width: parent.width
                indeterminate: true
                visible: model.installing
            }

            onClicked: {
                if (model.installed || model.installing)
                    return
                if (model.verdict === "broken") {
                    // Never a hard block: the user is told what will happen and
                    // then gets to decide.
                    remorseAction(
                        //% "Installing %1 — it will not work here"
                        qsTrId("atlantic-la-extension_installing_broken").arg(model.name),
                        function() { WebExtensionStore.install(model.slug) })
                } else {
                    WebExtensionStore.install(model.slug)
                }
            }
        }

        ViewPlaceholder {
            enabled: listView.count === 0 && !WebExtensionStore.busy
            //% "Nothing to show"
            text: qsTrId("atlantic-la-extension_store_empty")
            //% "Search addons.mozilla.org to find extensions"
            hintText: qsTrId("atlantic-la-extension_store_empty_hint")
        }

        VerticalScrollDecorator {}
    }

    Timer {
        id: searchTimer
        interval: 500
        onTriggered: WebExtensionStore.search(page.pendingQuery)
    }

    Component {
        id: linkDialog

        Dialog {
            property string link

            Column {
                width: parent.width

                DialogHeader {
                    //% "Install"
                    acceptText: qsTrId("atlantic-he-extension_install_accept")
                }

                TextField {
                    width: parent.width
                    //% "AMO address, add-on name, or a .xpi link"
                    placeholderText: qsTrId("atlantic-la-extension_link_placeholder")
                    //% "Install from a link"
                    label: qsTrId("atlantic-la-extension_link_label")
                    inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    onTextChanged: link = text
                    EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                    EnterKey.onClicked: accept()
                }

                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: Theme.secondaryColor
                    //% "A direct .xpi link is downloaded as given and cannot be checked against a publisher checksum."
                    text: qsTrId("atlantic-la-extension_link_warning")
                }
            }

            onAccepted: WebExtensionStore.installFromUserInput(link)
        }
    }

    Notice {
        id: storeNotice

        duration: 3000
        verticalOffset: -Theme.paddingLarge
    }

    Connections {
        target: WebExtensionStore

        onInstalled: {
            //% "Installed %1"
            storeNotice.text = qsTrId("atlantic-la-extension_installed_notice").arg(name)
            storeNotice.show()
        }
        onInstallFailed: {
            //% "Could not install %1"
            storeNotice.text = error !== ""
                ? error
                : qsTrId("atlantic-la-extension_install_failed_notice").arg(slug)
            storeNotice.show()
        }
    }
}
