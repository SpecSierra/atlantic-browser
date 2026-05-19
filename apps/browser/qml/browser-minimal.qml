import QtQuick 2.2
import QtQuick.Window 2.2
import Sailfish.Browser 1.0

Rectangle {
    id: root

    width: Screen.width
    height: Screen.height
    color: "black"

    readonly property string currentTitle: webView.title && webView.title.length > 0
                                           ? webView.title
                                           : (webView.url && webView.url.length > 0
                                              ? webView.url
                                              : "Atlantic")

    Rectangle {
        id: header

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 72
        color: "#202020"
        z: 2

        Text {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            verticalAlignment: Text.AlignVCenter
            color: "white"
            elide: Text.ElideRight
            font.pixelSize: 24
            text: root.currentTitle
        }

        Rectangle {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            height: 4
            width: parent.width * (webView.loadProgress / 100.0)
            color: "#2d8cff"
            visible: webView.loading
        }
    }

    WebContainer {
        id: webView

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        foreground: Qt.application.state === Qt.ApplicationActive
        maxLiveTabCount: 3
        readyToPaint: true
    }

    Connections {
        target: WebUtils

        onOpenUrlRequested: {
            if (url && url.length > 0) {
                webView.load(url)
            } else if (WebUtils.homePage && WebUtils.homePage.length > 0) {
                webView.load(WebUtils.homePage)
            } else {
                webView.load("https://jolla.com")
            }
        }
    }
}
