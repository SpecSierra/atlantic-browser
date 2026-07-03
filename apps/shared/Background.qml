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
import "WallpaperUtils.js" as WallpaperUtils

Item {
    id: wallpaper
    clip: true

    // Lower divisor = sharper, higher = stronger frosted blur. Keep it modest so
    // the blurred texture stays high-res enough not to look pixelated.
    property int blurDownscale: 4

    property string wpUrl: WallpaperUtils.ambienceImageUrl(Ambience.source)

    // Full-screen ambience wallpaper, rendered off-screen. It is captured only;
    // it is NOT positioned relative to this Background item, so that the chrome
    // can sample it in fixed screen-space coordinates (see gl_FragCoord below).
    Image {
        id: wpImage
        width: Screen.width
        height: Screen.height
        fillMode: Image.PreserveAspectCrop
        source: wallpaper.wpUrl
        asynchronous: false
        cache: true
        layer.enabled: true
        layer.textureSize: Qt.size(Screen.width, Screen.height)
    }

    // Heavy downscale before blurring: the ~24x downsample (bilinearly upscaled)
    // already smears the wallpaper into smooth colour fields, and FastBlur on top
    // finishes the frosted-glass look matching native SFOS chrome. Far stronger
    // than FastBlur's max radius (64) alone, and cheap to compute per frame.
    ShaderEffectSource {
        id: wpCapture
        sourceItem: wpImage
        textureSize: Qt.size(Math.max(1, Screen.width / wallpaper.blurDownscale),
                             Math.max(1, Screen.height / wallpaper.blurDownscale))
        hideSource: true
        live: true
    }

    // Two stacked blur passes compound into a strong, smooth blur. Chained
    // directly (FastBlur sourcing FastBlur) — do NOT insert a ShaderEffectSource
    // between them: each SES capture flips vertically, and an odd number of flips
    // renders the wallpaper upside down (e.g. in the popup menu).
    FastBlur {
        id: wpBlur1
        width: Screen.width
        height: Screen.height
        source: wpCapture
        radius: 64
        visible: false
    }

    FastBlur {
        id: wpBlur
        width: Screen.width
        height: Screen.height
        source: wpBlur1
        radius: 64
        visible: false
    }

    // Compositing pass: blurred wallpaper (screen-fixed) + dark tint. Sampling by
    // gl_FragCoord pins the wallpaper to the screen so the moving chrome element
    // only "reveals" it.
    ShaderEffect {
        id: wallpaperEffect
        anchors.fill: parent
        blending: true

        property variant wpTexture: ShaderEffectSource {
            hideSource: true
            sourceItem: wpBlur
            live: true
        }
        property size screenSize: Qt.size(Screen.width, Screen.height)

        // Tint applied over the blurred wallpaper (rgb mixed by alpha) — dark on
        // dark ambiences, light on light ambiences so black text stays readable.
        // Light scheme needs a stronger tint: black text wants a near-white
        // surface, not a pastel wash of the wallpaper.
        property color tint: Theme.colorScheme === Theme.DarkOnLight
                             ? Qt.rgba(1.0, 1.0, 1.0, 0.93)
                             : Qt.rgba(0.0, 0.0, 0.0, 0.62)

        vertexShader: "
            uniform highp mat4 qt_Matrix;
            attribute highp vec4 qt_Vertex;
            void main() {
                gl_Position = qt_Matrix * qt_Vertex;
            }
        "

        fragmentShader: "
            uniform sampler2D wpTexture;
            uniform highp vec2 screenSize;
            uniform lowp vec4 tint;
            uniform lowp float qt_Opacity;
            void main() {
                highp vec2 wpCoord = vec2(gl_FragCoord.x / screenSize.x,
                                          1.0 - gl_FragCoord.y / screenSize.y);
                lowp vec4 c = texture2D(wpTexture, wpCoord);
                c.rgb = mix(c.rgb, tint.rgb, tint.a);
                c.a = 1.0;
                gl_FragColor = c * qt_Opacity;
            }
        "
    }

    // Fine glass grain on top, exactly like native SFOS chrome surfaces.
    Image {
        anchors.fill: parent
        source: "image://theme/graphic-shader-texture"
        fillMode: Image.Tile
        // The grain darkens white surfaces, so fade it in the light scheme.
        opacity: Theme.colorScheme === Theme.DarkOnLight ? 0.06 : 0.15
    }
}
