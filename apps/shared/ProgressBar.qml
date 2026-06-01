/****************************************************************************
**
** Copyright (c) 2013 Jolla Ltd.
** Contact: Vesa-Matti Hartikainen <vesa-matti.hartikainen@jollamobile.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import Sailfish.Silica 1.0

Item {
    id: progressBar

    // From 0 to 1.0
    property real progress: 0.0

    // Thin accent line at the top edge of the bounding box —
    // it appears as a slim indicator at the chrome/content boundary.
    readonly property int _h: Math.max(3, Math.round(3 * Theme.pixelRatio))

    Item {
        id: track

        anchors.top: parent.top
        width: parent.width
        height: progressBar._h

        // Faint background rail
        Rectangle {
            anchors.fill: parent
            color: Theme.highlightColor
            opacity: 0.18
        }

        // Filled portion — clipped to current progress width
        Item {
            id: fill

            anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
            width: progressBar.progress * track.width
            clip: true

            Behavior on width {
                enabled: progressBar.opacity === 1.0
                SmoothedAnimation { velocity: 500; duration: 250 }
            }

            // Solid accent fill
            Rectangle {
                anchors.fill: parent
                color: Theme.highlightColor
            }

            // Shimmer stripe travelling across the fill
            Rectangle {
                id: shimmer

                width: track.width * 0.3
                height: progressBar._h
                color: "white"
                opacity: 0.38

                SequentialAnimation on x {
                    running: progressBar.opacity > 0
                    loops: Animation.Infinite
                    NumberAnimation {
                        from: -shimmer.width
                        to: track.width
                        duration: 1400
                        easing.type: Easing.InOutSine
                    }
                    PauseAnimation { duration: 600 }
                }
            }
        }
    }

    Behavior on opacity { FadeAnimation {} }
}
