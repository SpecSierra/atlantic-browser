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

    SilicaBackground.Background {
        anchors.fill: parent

        layer.enabled: true
        layer.effect: ShaderEffect {
            property size texSizeInv: Qt.size(1.0 / width, 1.0 / height)
            property real radius: 6.0

            fragmentShader: "
                uniform lowp sampler2D source;
                uniform highp vec2 texSizeInv;
                uniform highp float radius;
                varying highp vec2 qt_TexCoord0;
                uniform lowp float qt_Opacity;
                void main() {
                    lowp vec4 sum = vec4(0.0);
                    float r = radius;
                    for (float x = -r; x <= r; x += 1.0) {
                        for (float y = -r; y <= r; y += 1.0) {
                            sum += texture2D(source, qt_TexCoord0 + vec2(x * texSizeInv.x, y * texSizeInv.y));
                        }
                    }
                    float cnt = (r * 2.0 + 1.0) * (r * 2.0 + 1.0);
                    lowp vec4 c = texture2D(source, qt_TexCoord0);
                    gl_FragColor = mix(sum / cnt, c, 0.3) * qt_Opacity;
                }
            "
        }
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
