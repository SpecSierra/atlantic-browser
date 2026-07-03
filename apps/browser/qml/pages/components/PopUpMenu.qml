/*
 * Copyright (c) 2021 Open Mobile Platform LLC.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

import QtQuick 2.2
import QtGraphicalEffects 1.0
import Sailfish.Silica 1.0
import Sailfish.Silica.Background 1.0 as Background
import Sailfish.Silica.private 1.0 as Private
import Sailfish.Ambience 1.0
import "." as Components
import "../../shared/WallpaperUtils.js" as WallpaperUtils

SilicaControl {
    id: popUpMenu

    property var menuItem
    property var footer
    property alias active: menuLoader.active
    // Vertical nudge for the frosted-glass sampling, so the menu blur lines up with
    // the (toolbar-height-shorter) start page behind it. Set by the host page.
    property real blurOffsetY: 0
    // Height the wallpaper is cropped to, so the menu's blur matches the backdrop
    // behind it (the start page scales the wallpaper to its own, shorter, height).
    property real wallpaperHeight: height
    property int horizontalMargin: Math.max(Theme.paddingMedium, _biggestCorner * 0.5)
    property int verticalMargin: horizontalMargin
    readonly property int cornerRadius: 12
    readonly property int widthRatio: 18
    readonly property int heightRatio: 28

    property int _biggestCorner: Math.max(Screen.topLeftCorner.radius,
                                          Screen.topRightCorner.radius,
                                          Screen.bottomLeftCorner.radius,
                                          Screen.bottomRightCorner.radius)

    signal closed

    Private.AnimatedLoader {
        id: menuLoader

        width: popUpMenu.width
        height: popUpMenu.height

        animating: menuAnimation.running

        active: false
        source: menuComponent

        onAnimate: {
            if (item) {
                item.percentageClosed = 1
                menuAnimation.target = item
                menuAnimation.from = item.percentageClosed
                menuAnimation.to = 0
                menuAnimation.restart()
            } else if (replacedItem) {
                menuAnimation.target = replacedItem
                menuAnimation.from = 0
                menuAnimation.to = 1
                menuAnimation.restart()
            }
        }
    }

    NumberAnimation {
        id: menuAnimation

        running: false
        duration: 200
        easing.type: Easing.InOutQuad
        property: "percentageClosed"
    }

    // Blurred ambience wallpaper, in the menu's own coordinate space, for the
    // FrostedBox glass to sample by region (FBO-immune, unlike gl_FragCoord).
    property string _wpUrl: WallpaperUtils.ambienceImageUrl(Ambience.source)

    // The raw wallpaper must be captured but never drawn (hideSource is unreliable
    // here), so keep it inside a zero-size clipped container; the capture SES below
    // has no layout size so it doesn't draw either — only its textureSize matters.
    Item {
        width: 0
        height: 0
        clip: true
        Image {
            id: menuWp
            width: popUpMenu.width
            height: popUpMenu.wallpaperHeight
            source: popUpMenu._wpUrl
            fillMode: Image.PreserveAspectCrop
            asynchronous: false
            cache: true
            layer.enabled: true
            layer.textureSize: Qt.size(Math.max(1, popUpMenu.width), Math.max(1, popUpMenu.wallpaperHeight))
        }
    }

    ShaderEffectSource {
        id: menuWpCapture
        sourceItem: menuWp
        textureSize: Qt.size(Math.max(1, popUpMenu.width), Math.max(1, popUpMenu.wallpaperHeight))
        hideSource: true
        live: popUpMenu.active
    }

    FastBlur {
        id: menuBlur1
        width: popUpMenu.width
        height: popUpMenu.wallpaperHeight
        source: menuWpCapture
        radius: 64
        visible: false
    }

    FastBlur {
        id: menuBlur
        width: popUpMenu.width
        height: popUpMenu.wallpaperHeight
        source: menuBlur1
        radius: 64
        visible: false
    }

    Component {
        id: menuComponent

        Rectangle {
            id: menuItem

            property real percentageClosed
            readonly property real menuTop: Math.max(0, headerItem.y - menuFlickable.contentY)
            readonly property real topPadding: Theme.paddingLarge

            width: popUpMenu.width
            height: popUpMenu.height

            color: Theme.rgba(Theme.colorScheme === Theme.DarkOnLight ? "white" : "black",
                              Theme.opacityLow * (1 - percentageClosed))

            Flickable {
                id: menuFlickable

                x: popUpMenu.width - width
                y: menuItem.percentageClosed * popUpMenu.height

                width: Math.max(Theme.paddingLarge * widthRatio,
                                footerLoader.item ? footerLoader.item.implicitWidth : 0)
                height: popUpMenu.height

                contentHeight: menuItem.topPadding + headerItem.height + contentLoader.height + footerLoader.height

                interactive: popUpMenu.active   // Don't handle mouse events during fade out.

                flickableDirection: Flickable.VerticalFlick
                boundsBehavior: Flickable.DragOverBounds

                onDragEnded: {
                    if (contentY < -popUpMenu.verticalMargin - Theme.paddingLarge) {
                        topMargin = -contentY
                        popUpMenu.closed()
                    }
                }

                Item {
                    id: headerItem

                    y: menuItem.topPadding
                        + Math.min(0, menuFlickable.contentY)
                        - Math.min(0, menuFlickable.contentHeight - menuFlickable.height - menuFlickable.contentY)

                    width: menuFlickable.width
                    height: Theme.paddingLarge
                }

                Private.AnimatedLoader {
                    id: contentLoader

                    y: headerItem.y + headerItem.height

                    width: menuFlickable.width
                    height: item ? item.height : 0
                    source: popUpMenu.menuItem

                    onInitializeItem: {
                        item.width = Qt.binding(function() { return menuFlickable.width })
                    }
                }

                children: [
                    Components.FrostedBox {
                        id: background

                        blurSource: menuBlur
                        alignParent: popUpMenu
                        sampleDY: popUpMenu.blurOffsetY
                        radius: 0
                        tintAlpha: 0.6
                        y: Math.max(0, headerItem.y - menuFlickable.contentY)
                        z: -1
                        width: footerLoader.width
                        height: footerLoader.y - y
                    },
                    Item {
                        id: decoratorParent

                        y: background.y + headerItem.height
                        width: footerLoader.width
                        height: footerLoader.y - y

                        VerticalScrollDecorator {
                            _forcedParent: parent
                            flickable: menuFlickable

                            _sizeRatio: decoratorParent.height / (menuFlickable.contentHeight - contentLoader.y - footerLoader.height)
                            y: menuFlickable.contentHeight > menuFlickable.height + headerItem.y
                                    ? ((decoratorParent.height - height)
                                        * Math.max(0, menuFlickable.contentY - headerItem.y)
                                        / (menuFlickable.contentHeight - menuFlickable.height - headerItem.y))
                                    : 0
                        }
                    },
                    Components.FrostedBox {
                        blurSource: menuBlur
                        alignParent: popUpMenu
                        sampleDY: popUpMenu.blurOffsetY
                        radius: 0
                        tintAlpha: 0.6
                        y: Math.max(0, headerItem.y - menuFlickable.contentY)
                        width: headerItem.width
                        height: headerItem.height + Theme.paddingMedium

                        Rectangle {
                            x: (headerItem.width - width) / 2
                            y: (headerItem.height - height)

                            width: Theme.itemSizeLarge
                            height: Theme.paddingSmall

                            radius: height / 2

                            color: popUpMenu.palette.primaryColor

                            opacity: menuFlickable.contentY < headerItem.y ? 1 : Theme.opacityLow
                        }
                    },
                    MouseArea {
                        anchors.fill: footerLoader
                    },
                    Private.AnimatedLoader {
                        id: footerLoader

                        y: menuFlickable.height - height

                        width: menuFlickable.width
                        height: item ? item.height: 0

                        source: popUpMenu.footer

                        onInitializeItem: {
                            item.width = Qt.binding(function() { return menuFlickable.width })
                            // Wire the footer's frosted glass (PopUpMenuFooter is a
                            // FrostedBox). hasOwnProperty misses QML-declared props,
                            // so test the property value instead.
                            if (item.blurSource !== undefined) {
                                item.blurSource = menuBlur
                                item.alignParent = popUpMenu
                                item.sampleDY = Qt.binding(function() { return popUpMenu.blurOffsetY })
                            }
                        }
                    }
                ]
            }

            ShaderEffectSource {
                id: menuShaderSource

                x: menuFlickable.x
                y: menuFlickable.y
                width: menuFlickable.width
                height: menuFlickable.height

                sourceItem: menuFlickable
                hideSource: true
            }

            Background.Background {
                id: menuShaderItem

                x: menuShaderSource.x
                y: menuShaderSource.y
                width: menuShaderSource.width
                height: menuShaderSource.height

                radius: 0
                sourceItem: menuShaderSource
                fillMode: Background.Background.Stretch
            }

            InverseMouseArea {
                anchors.fill: menuShaderItem
                enabled: popUpMenu.active
                stealPress: true
                onPressedOutside: closed()
            }
        }
    }
}
