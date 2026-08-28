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
    // A folder tile was tapped: the bookmarks page takes over from here, since
    // the start page stays pinned to the one folder it is configured with.
    signal openFolder(string folderId, string title)
    // The "+" tile: add a link to the grid. It files a link rather than
    // navigating to one, which is what the address bar is for.
    signal addFavorite()

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

            // Index of the tile being dragged, -1 when idle, and the slot it
            // is currently hovering over. Held here rather than in the delegate
            // because every delegate needs to see them.
            //
            // The model is NOT reordered while dragging. QQuickRepeater
            // regenerates its delegates on a model change, which destroys the
            // very item holding the mouse grab: the release event is then never
            // delivered, the drag state is stranded, and the tile is left
            // frozen wherever it was. Tiles are displaced visually instead and
            // the model is written once, on release.
            property int dragIndex: -1
            property int hoverIndex: -1
            property bool overBin: false
            // Finger position in the grid's own coordinates. The dragged tile
            // is positioned FROM this rather than from accumulated mouse
            // deltas, which drift off the finger as the layout shifts.
            property real pointerX: 0
            property real pointerY: 0

            readonly property int strideX: cell + spacing

            // Where a tile should sit right now: everything between the dragged
            // tile's origin and the slot it hovers over shuffles up or down one
            // place to open a gap.
            function slotFor(index) {
                if (dragIndex < 0 || hoverIndex < 0 || dragIndex === hoverIndex)
                    return index
                if (index === dragIndex)
                    return hoverIndex
                if (dragIndex < hoverIndex && index > dragIndex && index <= hoverIndex)
                    return index - 1
                if (hoverIndex < dragIndex && index >= hoverIndex && index < dragIndex)
                    return index + 1
                return index
            }

            // Which slot a point over the grid belongs to. The grid is uniform,
            // so this is arithmetic rather than hit-testing -- and it has to
            // clamp, because a drag ranges outside the laid-out cells.
            function slotAt(x, y) {
                var col = Math.max(0, Math.min(columns - 1, Math.floor(x / strideX)))
                var row = Math.max(0, Math.floor(y / strideX))
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

                    // Displacement from this tile's natural cell to the slot
                    // it should currently occupy. Bindings, not assignments:
                    // if an animation is ever cut short the binding still
                    // re-evaluates to the right place, so a tile cannot be left
                    // stranded the way a Positioner transition leaves one.
                    readonly property int slot: grid.slotFor(index)
                    // Not readonly: a Behavior assigns to the property it
                    // animates, and QML rejects that on a readonly one.
                    property real offsetX: dragging
                            ? grid.pointerX - (cellItem.x + grid.cell / 2)
                            : ((slot % grid.columns) - (index % grid.columns)) * grid.strideX
                    property real offsetY: dragging
                            ? grid.pointerY - (cellItem.y + cellItem.height / 2)
                            : (Math.floor(slot / grid.columns) - Math.floor(index / grid.columns))
                              * (cellItem.height + grid.spacing)

                    transform: Translate {
                        x: cellItem.offsetX
                        y: cellItem.offsetY
                    }

                    // Always enabled: offsets are 0 unless a drag is in
                    // progress, so nothing animates when the grid is merely
                    // populated. The dragged tile is exempt -- it must track
                    // the finger 1:1, not chase it.
                    Behavior on offsetX {
                        enabled: !cellItem.dragging
                        NumberAnimation { duration: 150; easing.type: Easing.OutQuad }
                    }
                    Behavior on offsetY {
                        enabled: !cellItem.dragging
                        NumberAnimation { duration: 150; easing.type: Easing.OutQuad }
                    }

                    scale: dragging ? 1.12 : 1.0
                    Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutQuad } }

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

                            anchors.fill: parent

                            function trackPointer(mouse) {
                                var p = mapToItem(grid, mouse.x, mouse.y)
                                grid.pointerX = p.x
                                grid.pointerY = p.y
                            }

                            onClicked: {
                                if (model.isFolder)
                                    root.openFolder(model.bookmarkId, model.title)
                                else
                                    root.loadUrl(model.url, true)
                            }

                            onPressed: trackPointer(mouse)

                            onPressAndHold: {
                                trackPointer(mouse)
                                grid.dragIndex = index
                                grid.hoverIndex = index
                            }

                            onPositionChanged: {
                                if (grid.dragIndex !== index)
                                    return

                                trackPointer(mouse)

                                grid.overBin = grid.pointerY > grid.height + Theme.paddingLarge
                                if (!grid.overBin)
                                    grid.hoverIndex = grid.slotAt(grid.pointerX, grid.pointerY)
                            }

                            onReleased: {
                                var from = grid.dragIndex
                                var to = grid.hoverIndex
                                var binned = grid.overBin

                                // Clear first: committing the move regenerates
                                // the delegates, and this handler's own item
                                // goes with them.
                                grid.dragIndex = -1
                                grid.hoverIndex = -1
                                grid.overBin = false

                                if (from < 0)
                                    return

                                if (binned) {
                                    //% "Removing bookmark"
                                    removeRemorse.execute(qsTrId("atlantic-la-removing_bookmark"),
                                                          function() { favoritesModel.removeAt(from) })
                                } else if (to >= 0 && to !== from) {
                                    favoritesModel.move(from, to)
                                }
                            }

                            onCanceled: {
                                grid.dragIndex = -1
                                grid.hoverIndex = -1
                                grid.overBin = false
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
                        onClicked: root.addFavorite()
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
