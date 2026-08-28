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
import Sailfish.Browser 1.0
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
    // A folder tile was tapped: the bookmarks page takes over from here, since
    // the start page stays pinned to the one folder it is configured with.
    signal openFolder(string folderId, string title)

    // Which bookmark folder the quick links show. "" is the root, which is
    // where every bookmark lives until folders are used.
    property string startFolderId: ""

    BookmarkFolderModel {
        id: favoritesModel

        sourceModel: root.bookmarkModel
        folderId: root.startFolderId
    }

    // true while the address-bar overlay is open (hide the foreground then)
    property bool overlayOpen: false

    // Height of the visible area above the toolbar, used to lay out the foreground
    // (clock / search / grid). The wallpaper backdrop, in contrast, fills the whole
    // item (full screen) so the toolbar strip keeps wallpaper behind it — otherwise,
    // when the popup menu hides the toolbar, that strip falls through to the empty
    // white web view. Defaults to the item height when the host doesn't set it.
    property real contentHeight: height

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
            id: scrimGradient
            // Scrim brightness follows the ambience scheme: dark under light
            // text, light under black text (light/white ambiences).
            property real level: Theme.colorScheme === Theme.DarkOnLight ? 1 : 0
            GradientStop { position: 0.0; color: Qt.rgba(scrimGradient.level, scrimGradient.level, scrimGradient.level, 0.6) }
            GradientStop { position: 0.45; color: Qt.rgba(scrimGradient.level, scrimGradient.level, scrimGradient.level, 0.3) }
            GradientStop { position: 1.0; color: Qt.rgba(scrimGradient.level, scrimGradient.level, scrimGradient.level, 0.65) }
        }
    }

    // All foreground elements hide while the address-bar overlay is open; the
    // wallpaper + scrim stay so nothing white/black flashes through.
    Column {
        id: content

        width: parent.width - 2 * Theme.horizontalPageMargin
        anchors.horizontalCenter: parent.horizontalCenter
        // Position within the visible area (above the toolbar), not the full-screen
        // item height — so the foreground stays put while the backdrop fills the screen.
        y: Math.round(root.contentHeight * 0.14)
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

        // Quick links: the contents of one chosen bookmark folder, in the
        // user's own order. Tiles are drag-reorderable, and dragging one down
        // onto the bin removes it -- press-and-hold used to delete outright,
        // which is now the gesture that picks a tile up instead.
        Grid {
            id: grid

            anchors.horizontalCenter: parent.horizontalCenter
            columns: 4
            spacing: Theme.paddingLarge

            readonly property int cell: Math.floor((content.width - (columns - 1) * Theme.paddingLarge) / columns)
            readonly property int tileSize: Math.round(cell * 0.82)
            readonly property int stride: cell + spacing

            // Index of the tile being dragged, -1 when idle. Held here rather
            // than in the delegate because every delegate needs to see it.
            property int dragIndex: -1
            property bool overBin: false

            // Which slot a point over the grid belongs to. The grid is uniform,
            // so this is arithmetic rather than hit-testing -- and it has to
            // clamp, because a drag ranges outside the laid-out cells.
            function slotAt(x, y) {
                var col = Math.max(0, Math.min(columns - 1, Math.floor(x / stride)))
                var row = Math.max(0, Math.floor(y / stride))
                return Math.max(0, Math.min(favoritesModel.count - 1, row * columns + col))
            }

            Repeater {
                model: favoritesModel

                delegate: Column {
                    id: cellItem

                    readonly property bool dragging: grid.dragIndex === index

                    width: grid.cell
                    spacing: Theme.paddingSmall
                    z: dragging ? 10 : 0
                    opacity: dragging && grid.overBin ? 0.4 : 1.0

                    // Follows the finger while dragging; the Repeater keeps
                    // re-laying the cells out underneath as rows move.
                    transform: Translate {
                        x: cellItem.dragging ? tileArea.dragDX : 0
                        y: cellItem.dragging ? tileArea.dragDY : 0
                    }

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
                            visible: !model.isFolder
                            icon: model.favicon
                            width: Math.round(grid.tileSize * 0.5)
                            height: width
                            sourceSize.width: width
                            sourceSize.height: width
                        }

                        // A folder filed inside the start folder: opening it
                        // hands over to the bookmarks page, since the start
                        // page itself stays pinned to its one folder.
                        Image {
                            anchors.centerIn: parent
                            visible: model.isFolder
                            width: Math.round(grid.tileSize * 0.45)
                            height: width
                            source: "image://theme/icon-m-folder?" + Theme.primaryColor
                        }

                        MouseArea {
                            id: tileArea

                            property real pressX
                            property real pressY
                            property real dragDX
                            property real dragDY

                            anchors.fill: parent

                            onClicked: {
                                if (model.isFolder)
                                    root.openFolder(model.bookmarkId, model.title)
                                else
                                    root.loadUrl(model.url, true)
                            }

                            onPressed: {
                                pressX = mouse.x
                                pressY = mouse.y
                                dragDX = 0
                                dragDY = 0
                            }

                            onPressAndHold: grid.dragIndex = index

                            onPositionChanged: {
                                if (grid.dragIndex !== index)
                                    return

                                dragDX = mouse.x - pressX
                                dragDY = mouse.y - pressY

                                var p = mapToItem(grid, mouse.x, mouse.y)
                                grid.overBin = p.y > grid.height + Theme.paddingLarge
                                if (grid.overBin)
                                    return

                                var target = grid.slotAt(p.x, p.y)
                                if (target !== index) {
                                    favoritesModel.move(index, target)
                                    // The delegate follows its row, so the drag
                                    // has to follow it to the new index or the
                                    // next move would come from the wrong one.
                                    grid.dragIndex = target
                                    // The cell moved under the finger; re-base
                                    // the offset so the tile does not jump.
                                    pressX = mouse.x - dragDX
                                    pressY = mouse.y - dragDY
                                }
                            }

                            onReleased: {
                                if (grid.dragIndex === index && grid.overBin) {
                                    var i = index
                                    //% "Removing bookmark"
                                    removeRemorse.execute(qsTrId("atlantic-la-removing_bookmark"),
                                                          function() { favoritesModel.removeAt(i) })
                                }
                                grid.dragIndex = -1
                                grid.overBin = false
                                dragDX = 0
                                dragDY = 0
                            }

                            onCanceled: {
                                grid.dragIndex = -1
                                grid.overBin = false
                                dragDX = 0
                                dragDY = 0
                            }
                        }
                    }

                    Label {
                        width: grid.cell
                        horizontalAlignment: Text.AlignHCenter
                        text: model.isFolder ? model.title : WebUtils.displayableUrl(model.url)
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
                visible: grid.dragIndex < 0

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
                    //% "Add"
                    text: qsTrId("atlantic-la-add")
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                }
            }
        }

        // Drop target, only while a tile is in the air.
        Item {
            width: 1
            height: grid.dragIndex >= 0 ? Theme.paddingLarge : 0
        }

        Browser.FrostedBox {
            anchors.horizontalCenter: parent.horizontalCenter
            width: Theme.itemSizeExtraLarge
            height: Theme.itemSizeMedium
            radius: Theme.paddingMedium
            blurSource: bgBlur
            alignParent: root
            visible: grid.dragIndex >= 0
            opacity: grid.overBin ? 1.0 : Theme.opacityHigh

            Image {
                anchors.centerIn: parent
                width: Theme.iconSizeMedium
                height: width
                source: "image://theme/icon-m-delete?"
                        + (grid.overBin ? Theme.highlightColor : Theme.primaryColor)
            }
        }
    }
}
