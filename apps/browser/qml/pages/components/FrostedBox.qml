/*
 * A rounded frosted-glass box that blurs the start page's own wallpaper aligned
 * to whatever sits directly behind it (sampled by region, so it matches the
 * background image instead of the screen-fixed chrome glass).
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

import QtQuick 2.2
import QtGraphicalEffects 1.0
import Sailfish.Silica 1.0

Item {
    id: box

    // The (already blurred) full-page wallpaper to sample from.
    property Item blurSource
    // The item whose coordinate space blurSource fills (the start page root).
    property Item alignParent
    property real radius: Theme.paddingMedium
    property real tintAlpha: 0.5
    // Light ambiences flip text to black, so the glass tint (and the edge
    // highlight/press flash) must flip to white/black accordingly.
    readonly property bool _lightScheme: Theme.colorScheme === Theme.DarkOnLight
    property bool pressed: false
    // Extra offset (px) applied to the sampled region, to line the blur up with a
    // differently-cropped backdrop (e.g. the popup menu vs the start page).
    property real sampleDX: 0
    property real sampleDY: 0

    default property alias content: contentArea.data

    Item {
        id: glass
        anchors.fill: parent
        layer.enabled: true
        layer.effect: OpacityMask {
            maskSource: Rectangle {
                width: box.width
                height: box.height
                radius: box.radius
                visible: false
            }
        }

        // The slice of the blurred wallpaper directly behind this box.
        ShaderEffectSource {
            anchors.fill: parent
            sourceItem: box.blurSource
            live: true
            sourceRect: {
                // Reference the layout drivers so the binding re-evaluates when the
                // box (or the page) is laid out / rotated, then map to align space.
                var driver = box.x + box.y + box.width + box.height + box.sampleDX + box.sampleDY
                        + (box.alignParent ? box.alignParent.width + box.alignParent.height : 0)
                var p = box.alignParent ? box.mapToItem(box.alignParent, 0, 0) : Qt.point(0, 0)
                return Qt.rect(p.x + box.sampleDX, p.y + box.sampleDY, box.width, box.height)
            }
        }

        Rectangle {
            anchors.fill: parent
            // In the light scheme the tint is boosted toward opaque white so the
            // glass reads as a bright surface under black text.
            color: box._lightScheme ? Qt.rgba(1, 1, 1, Math.min(1, box.tintAlpha + 0.25))
                                    : Qt.rgba(0, 0, 0, box.tintAlpha)
        }

        Image {
            anchors.fill: parent
            source: "image://theme/graphic-shader-texture"
            fillMode: Image.Tile
            opacity: box._lightScheme ? 0.05 : 0.12
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: box.radius
        color: box.pressed ? (box._lightScheme ? Qt.rgba(0, 0, 0, 0.14)
                                               : Qt.rgba(1, 1, 1, 0.14))
                           : "transparent"
        border.color: box._lightScheme ? Qt.rgba(0, 0, 0, 0.18)
                                       : Qt.rgba(1, 1, 1, 0.18)
        border.width: 1
    }

    Item {
        id: contentArea
        anchors.fill: parent
    }
}
