/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import Sailfish.Silica 1.0

// HTML <select> dropdown overlay — driven by the bound webView's contentItem
// selectMenu* property bindings. Fills the webView and dims/closes on tap.
Item {
    id: selectOverlay

    property var webView

    visible: webView && webView.contentItem !== null && webView.contentItem.selectMenuActive
    z: 1000
    anchors.fill: webView

    function dismiss() {
        if (webView.contentItem) webView.contentItem.closeSelectMenu()
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.highlightBackgroundColor
        opacity: 0.0
        MouseArea { anchors.fill: parent; onClicked: selectOverlay.dismiss() }
    }

    Rectangle {
        id: selectPanel
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
        height: Math.min(parent.height * 0.6, selectListView.contentHeight + Theme.itemSizeMedium)
        color: Theme.colorScheme === Theme.LightOnDark ? Qt.darker(Theme.overlayBackgroundColor, 1.2) : Theme.overlayBackgroundColor
        radius: Theme.paddingMedium

        Column {
            anchors.fill: parent

            Item {
                width: parent.width
                height: Theme.itemSizeMedium
                Label {
                    anchors.centerIn: parent
                    font.pixelSize: Theme.fontSizeLarge
                    color: Theme.highlightColor
                    text: qsTr("Select option")
                }
            }

            SilicaListView {
                id: selectListView
                width: parent.width
                height: selectPanel.height - Theme.itemSizeMedium
                clip: true
                model: selectOverlay.webView.contentItem ? selectOverlay.webView.contentItem.selectMenuOptions : []
                currentIndex: selectOverlay.webView.contentItem ? selectOverlay.webView.contentItem.selectMenuSelectedIndex : -1

                delegate: ListItem {
                    contentHeight: Theme.itemSizeSmall
                    highlighted: selectOverlay.webView.contentItem && index === selectOverlay.webView.contentItem.selectMenuSelectedIndex
                    Label {
                        text: modelData
                        color: (selectOverlay.webView.contentItem && index === selectOverlay.webView.contentItem.selectMenuSelectedIndex)
                               ? Theme.highlightColor : Theme.primaryColor
                        anchors {
                            verticalCenter: parent.verticalCenter
                            left: parent.left
                            leftMargin: Theme.horizontalPageMargin
                            right: parent.right
                            rightMargin: Theme.horizontalPageMargin
                        }
                        truncationMode: TruncationMode.Fade
                    }
                    onClicked: {
                        if (selectOverlay.webView.contentItem) selectOverlay.webView.contentItem.selectMenuOption(index)
                    }
                }
                ScrollDecorator {}
            }
        }
    }
}
