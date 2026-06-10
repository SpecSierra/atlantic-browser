/*
 * Atlantic Browser start page — shown when no tab is open.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

import QtQuick 2.2
import QtGraphicalEffects 1.0
import Sailfish.Silica 1.0
import Sailfish.Ambience 1.0
import "." as Browser
import "../../shared/WallpaperUtils.js" as WallpaperUtils

Item {
    id: root

    property var bookmarkModel
    property var historyModel

    // url, openInNewTab
    signal loadUrl(string url, bool newTab)
    // open the address-bar entry (keyboard + suggestions)
    signal openSearch()

    // true while the address-bar overlay is open (hide the foreground then)
    property bool overlayOpen: false

    property var now: new Date()

    property string wpUrl: WallpaperUtils.ambienceImageUrl(Ambience.source)

    Timer {
        interval: 20000
        repeat: true
        running: root.visible
        onTriggered: root.now = new Date()
    }
    onVisibleChanged: if (visible) now = new Date()

    RemorsePopup { id: removeRemorse }

    // Wallpaper backdrop (sharp)
    Image {
        id: bgImage
        anchors.fill: parent
        source: root.wpUrl
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        cache: true
    }

    // Blurred copy of the backdrop, in the same coordinate space, sampled by the
    // frosted boxes so their blur aligns with what's actually behind them. Kept at
    // full resolution (two stacked passes for strength) — the boxes upscale small
    // sub-regions, so a downscaled source would look pixelated.
    FastBlur {
        id: bgBlur1
        anchors.fill: parent
        source: bgImage
        radius: 64
        visible: false
    }

    FastBlur {
        id: bgBlur
        anchors.fill: parent
        source: bgBlur1
        radius: 64
        visible: false
    }

    // Legibility scrim
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0.6) }
            GradientStop { position: 0.45; color: Qt.rgba(0, 0, 0, 0.3) }
            GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.65) }
        }
    }

    // All foreground elements hide while the address-bar overlay is open; the
    // wallpaper + scrim stay so nothing white/black flashes through.
    Column {
        id: content

        width: parent.width - 2 * Theme.horizontalPageMargin
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.round(parent.height * 0.14)
        spacing: Theme.paddingMedium
        opacity: root.overlayOpen ? 0.0 : 1.0
        visible: opacity > 0.0
        Behavior on opacity { FadeAnimation { duration: 150 } }

        // Clock + date in a frosted glass box
        Browser.FrostedBox {
            id: clockBox

            anchors.horizontalCenter: parent.horizontalCenter
            width: clockColumn.width + 3 * Theme.paddingLarge
            height: clockColumn.height + 2 * Theme.paddingLarge
            radius: Theme.paddingLarge
            blurSource: bgBlur
            alignParent: root

            Column {
                id: clockColumn
                anchors.centerIn: parent
                spacing: Theme.paddingSmall

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: Qt.formatTime(root.now, "HH:mm")
                    color: Theme.primaryColor
                    font.pixelSize: Math.round(Theme.fontSizeHuge * 1.7)
                    font.weight: Font.Light
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: Qt.formatDate(root.now, "dddd, d MMMM")
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeMedium
                }
            }
        }

        Item { width: 1; height: Theme.paddingLarge }

        // Search bar — frosted glass, lightly rounded (Sailfish chrome style)
        Browser.FrostedBox {
            id: searchBar

            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width
            height: Theme.itemSizeMedium
            radius: Theme.paddingMedium
            blurSource: bgBlur
            alignParent: root
            pressed: searchArea.pressed

            Row {
                anchors.left: parent.left
                anchors.leftMargin: Theme.paddingLarge
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.paddingMedium

                Image {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.iconSizeSmall
                    height: Theme.iconSizeSmall
                    source: "image://theme/icon-m-search?" + Theme.primaryColor
                }

                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    //% "Search or type URL"
                    text: qsTrId("sailfish_browser-ph-type_url_or_search")
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeMedium
                }
            }

            MouseArea {
                id: searchArea
                anchors.fill: parent
                onClicked: root.openSearch()
            }
        }

        Item { width: 1; height: Theme.paddingLarge }

        // Quick links (bookmarks) + add tile
        Grid {
            id: grid

            anchors.horizontalCenter: parent.horizontalCenter
            columns: 4
            spacing: Theme.paddingLarge

            readonly property int cell: Math.floor((content.width - (columns - 1) * Theme.paddingLarge) / columns)
            readonly property int tileSize: Math.round(cell * 0.82)

            Repeater {
                model: root.bookmarkModel

                delegate: Column {
                    width: grid.cell
                    spacing: Theme.paddingSmall

                    Browser.FrostedBox {
                        id: tile
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: grid.tileSize
                        height: grid.tileSize
                        radius: width / 2
                        blurSource: bgBlur
                        alignParent: root
                        pressed: tileArea.pressed

                        Browser.FavoriteIcon {
                            anchors.centerIn: parent
                            icon: model.favicon
                            width: Math.round(grid.tileSize * 0.5)
                            height: width
                            sourceSize.width: width
                            sourceSize.height: width
                        }

                        MouseArea {
                            id: tileArea
                            anchors.fill: parent
                            onClicked: root.loadUrl(model.url, true)
                            onPressAndHold: {
                                var u = model.url
                                removeRemorse.execute("Removing bookmark",
                                                      function() { if (root.bookmarkModel) root.bookmarkModel.remove(u) })
                            }
                        }
                    }

                    Label {
                        width: grid.cell
                        horizontalAlignment: Text.AlignHCenter
                        text: WebUtils.displayableUrl(model.url)
                        truncationMode: TruncationMode.Fade
                        color: Theme.primaryColor
                        font.pixelSize: Theme.fontSizeExtraSmall
                    }
                }
            }

            // Add tile
            Column {
                width: grid.cell
                spacing: Theme.paddingSmall

                Browser.FrostedBox {
                    id: addTile
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: grid.tileSize
                    height: grid.tileSize
                    radius: width / 2
                    blurSource: bgBlur
                    alignParent: root
                    pressed: addArea.pressed

                    Image {
                        anchors.centerIn: parent
                        width: Math.round(grid.tileSize * 0.42)
                        height: width
                        source: "image://theme/icon-m-add?" + Theme.primaryColor
                    }

                    MouseArea {
                        id: addArea
                        anchors.fill: parent
                        onClicked: root.openSearch()
                    }
                }

                Label {
                    width: grid.cell
                    horizontalAlignment: Text.AlignHCenter
                    text: "Add"
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                }
            }
        }
    }
}
