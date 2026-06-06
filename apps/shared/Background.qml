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
import QtGraphicalEffects 1.0
import Sailfish.Silica 1.0
import Sailfish.Ambience 1.0
import "." as Browser

Item {
    id: wallpaper

    property string wpUrl: {
        var s = String(Ambience.source).replace("file://", "");
        var p = s.lastIndexOf("/");
        if (p < 0) return "file:///usr/share/ambience/fire/images/ambience_fire.jpg";
        var dir = s.substring(0, p);
        var name = s.substring(p + 1).replace(".ambience", "");
        return "file://" + dir + "/images/ambience_" + name + ".jpg";
    }

    Image {
        id: wpImage
        x: 0
        anchors.bottom: parent.bottom
        width: parent.width
        height: sourceSize.height > 0
                ? sourceSize.height * (parent.width / sourceSize.width)
                : parent.height
        source: wallpaper.wpUrl
    }

    FastBlur {
        id: wpBlur
        anchors.fill: parent
        source: wpImage
        radius: 24
    }

    ShaderEffect {
        id: blurOverlay
        anchors.fill: parent
        property var blur: wpBlur
        blending: true
        opacity: 0.55

        vertexShader: "
            uniform highp mat4 qt_Matrix;
            attribute highp vec4 qt_Vertex;
            attribute highp vec2 qt_MultiTexCoord0;
            varying highp vec2 qt_TexCoord0;
            void main() {
                qt_TexCoord0 = qt_MultiTexCoord0;
                gl_Position = qt_Matrix * qt_Vertex;
            }
        "

        fragmentShader: "
            uniform sampler2D blur;
            varying highp vec2 qt_TexCoord0;
            uniform lowp float qt_Opacity;
            void main() {
                gl_FragColor = texture2D(blur, qt_TexCoord0) * qt_Opacity;
            }
        "
    }

    Item {
        id: glassTextureItem

        visible: false
        width: glassTextureImage.width
        height: glassTextureImage.height

        Rectangle {
            color: Qt.darker(
                Qt.tint(
                    Theme.colorScheme === Theme.LightOnDark ? "#1c1c1c" : "#f2f2f2",
                    Qt.rgba(Theme.highlightColor.r,
                            Theme.highlightColor.g,
                            Theme.highlightColor.b,
                            0.52)),
                1.55)
            opacity: 0.95
            anchors.fill: parent
        }

        Image {
            id: glassTextureImage
            opacity: 0.25
            source: "image://theme/graphic-shader-texture"
        }
    }

    ShaderEffect {
        id: wallpaperEffect

        anchors.fill: parent
        opacity: 0.85

        property size glassTextureSizeInv: Qt.size(1.0 / (glassTextureImage.sourceSize.width),
                                                   -1.0 / (glassTextureImage.sourceSize.height))

        property variant glassTexture: ShaderEffectSource {
            hideSource: true
            sourceItem: glassTextureItem
            wrapMode: ShaderEffectSource.Repeat
        }

        blending: true

        vertexShader: "
           uniform highp mat4 qt_Matrix;
           attribute highp vec4 qt_Vertex;

           void main() {
              gl_Position = qt_Matrix * qt_Vertex;
           }
        "

        fragmentShader: "
           uniform sampler2D glassTexture;
           uniform highp vec2 glassTextureSizeInv;
           uniform lowp float qt_Opacity;

           void main() {
              lowp vec4 tx = texture2D(glassTexture, gl_FragCoord.xy * glassTextureSizeInv);
              gl_FragColor = tx * qt_Opacity;
           }
        "
    }
}
