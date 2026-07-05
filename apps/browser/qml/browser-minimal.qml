import QtQuick 2.2
import QtQuick.Window 2.2
import Sailfish.Browser 1.0
import Nemo.Configuration 1.0
import "pages/components/UrlUtils.js" as UrlUtils

Rectangle {
    id: root

    width: Screen.width
    height: Screen.height
    color: "#101010"

    property bool editingAddress: false

    readonly property string currentTitle: webView.title && webView.title.length > 0
                                           ? webView.title
                                           : (webView.url && webView.url.length > 0
                                              ? webView.url
                                              : "Atlantic")

    function normalizedInput(text) {
        return UrlUtils.normalize(text, SearchEngineModel.searchUrlTemplate(searchEngineConf.value))
    }

    ConfigurationValue {
        id: searchEngineConf
        key: "/apps/atlantic-browser/settings/search_engine"
        defaultValue: "Google"
    }

    ConfigurationValue {
        key: "/apps/atlantic-browser/settings/home_page"
        defaultValue: "http://jolla.com"
        // C++ can't read dconf (MDConfItem is a no-op stub); push the
        // persisted home page into WebUtils, same as BrowserPage.qml.
        onValueChanged: WebUtils.setHomePage(value)
        Component.onCompleted: WebUtils.setHomePage(value)
    }

    function loadAddress(text) {
        var target = normalizedInput(text)
        if (!target.length) {
            return
        }

        addressField.text = target
        editingAddress = false
        webView.load(target)
    }

    function syncAddressField() {
        if (!editingAddress && webView.url && webView.url.length > 0) {
            addressField.text = webView.url
        }
    }

    Rectangle {
        id: header

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 144
        color: "#171717"
        z: 2
    }

    Text {
        id: titleLabel

        anchors.left: header.left
        anchors.right: header.right
        anchors.top: header.top
        anchors.leftMargin: 18
        anchors.rightMargin: 18
        anchors.topMargin: 14
        color: "white"
        elide: Text.ElideRight
        font.pixelSize: 24
        font.bold: true
        text: root.currentTitle
        z: 3
    }

    Row {
        id: chromeRow

        anchors.left: header.left
        anchors.right: header.right
        anchors.top: titleLabel.bottom
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        anchors.topMargin: 14
        spacing: 10
        height: 54
        z: 3

        Rectangle {
            id: backAction

            width: 76
            height: parent.height
            radius: 10
            color: backMouseArea.pressed ? "#2f5f9f" : "#2a2a2a"
            border.width: 1
            border.color: "#5a5a5a"
            enabled: webView.canGoBack
            opacity: enabled ? 1.0 : 0.45

            Text {
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 26
                font.bold: true
                text: "<"
            }

            MouseArea {
                id: backMouseArea

                anchors.fill: parent
                enabled: backAction.enabled
                onClicked: webView.goBack()
            }
        }

        Rectangle {
            id: forwardAction

            width: 76
            height: parent.height
            radius: 10
            color: forwardMouseArea.pressed ? "#2f5f9f" : "#2a2a2a"
            border.width: 1
            border.color: "#5a5a5a"
            enabled: webView.canGoForward
            opacity: enabled ? 1.0 : 0.45

            Text {
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 26
                font.bold: true
                text: ">"
            }

            MouseArea {
                id: forwardMouseArea

                anchors.fill: parent
                enabled: forwardAction.enabled
                onClicked: webView.goForward()
            }
        }

        Rectangle {
            id: reloadAction

            width: 76
            height: parent.height
            radius: 10
            color: reloadMouseArea.pressed ? "#2f5f9f" : "#2a2a2a"
            border.width: 1
            border.color: "#5a5a5a"

            Text {
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 26
                font.bold: true
                text: webView.loading ? "X" : "\u21bb"
            }

            MouseArea {
                id: reloadMouseArea

                anchors.fill: parent
                onClicked: {
                    if (webView.loading) {
                        webView.stop()
                    } else {
                        webView.reload()
                    }
                }
            }
        }

        Rectangle {
            id: addressBackground

            height: parent.height
            radius: 10
            color: "#252525"
            border.width: 1
            border.color: addressField.activeFocus ? "#2d8cff" : "#555555"
            width: chromeRow.width
                   - backAction.width
                   - forwardAction.width
                   - reloadAction.width
                   - goAction.width
                   - chromeRow.spacing * 4

            TextInput {
                id: addressField

                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                verticalAlignment: TextInput.AlignVCenter
                color: "white"
                font.pixelSize: 22
                selectByMouse: true
                selectionColor: "#2d8cff"
                selectedTextColor: "white"
                inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase

                onActiveFocusChanged: {
                    editingAddress = activeFocus
                    if (activeFocus) {
                        selectAll()
                    } else {
                        root.syncAddressField()
                    }
                }

                Keys.onReturnPressed: root.loadAddress(text)
                Keys.onEnterPressed: root.loadAddress(text)
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                onPressed: {
                    if (!addressField.activeFocus) {
                        addressField.forceActiveFocus()
                    }
                    mouse.accepted = false
                }
            }
        }

        Rectangle {
            id: goAction

            width: 88
            height: parent.height
            radius: 10
            color: goMouseArea.pressed ? "#2f5f9f" : "#2a2a2a"
            border.width: 1
            border.color: "#5a5a5a"
            enabled: addressField.text.trim().length > 0
            opacity: enabled ? 1.0 : 0.45

            Text {
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 22
                font.bold: true
                text: "Go"
            }

            MouseArea {
                id: goMouseArea

                anchors.fill: parent
                enabled: goAction.enabled
                onClicked: root.loadAddress(addressField.text)
            }
        }
    }

    Rectangle {
        anchors.left: header.left
        anchors.bottom: header.bottom
        height: 4
        width: header.width * (webView.loadProgress / 100.0)
        color: "#2d8cff"
        visible: webView.loading
        z: 3
    }

    WebContainer {
        id: webView

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        maxLiveTabCount: 3
        readyToPaint: true

        onUrlChanged: root.syncAddressField()
    }

    Connections {
        target: WebUtils

        onOpenUrlRequested: {
            if (url && url.length > 0) {
                root.loadAddress(url)
            } else if (WebUtils.homePage && WebUtils.homePage.length > 0) {
                root.loadAddress(WebUtils.homePage)
            } else {
                root.loadAddress("https://jolla.com")
            }
        }
    }
}
