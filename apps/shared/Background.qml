/****************************************************************************
**
** Copyright (c) 2015 Jolla Ltd.
** Contact: Raine Makelainen <raine.makelainen@jolla.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import Sailfish.Silica 1.0
import Sailfish.Silica.Background 1.0 as SilicaBackground
import "." as Browser

Item {
    id: wallpaper

    Item {
        id: glassTextureItem

        width: glassTextureImage.width
        height: glassTextureImage.height

        Rectangle {
            // Ambience-derived chrome colour: blend the ambience accent into
            // a dark or light base so the bar is NEVER black but always
            // reflects the current ambience.
            color: Qt.darker(
                Qt.tint(
                    Theme.colorScheme === Theme.LightOnDark ? "#1c1c1c" : "#f2f2f2",
                    Qt.rgba(Theme.highlightColor.r,
                            Theme.highlightColor.g,
                            Theme.highlightColor.b,
                            0.52)),
                1.55)
            opacity: 0.70
            anchors.fill: parent
        }

        Image {
            id: glassTextureImage
            opacity: 0.50
            source: "image://theme/graphic-shader-texture"
        }
    }

    SilicaBackground.Background {
        anchors.fill: parent
        opacity: 0.85

        sourceItem: glassTextureItem
        fillMode: SilicaBackground.Background.Tile
    }
}
