/****************************************************************************
**
** Copyright (c) 2014 Jolla Ltd.
** Copyright (c) 2026 Atlantic Browser
** Contact: Raine Makelainen <raine.makelainen@jolla.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import Sailfish.Silica 1.0

/*
 * Cover for "no tabs open". The stock CoverPlaceholder blew the launcher icon
 * up to half the tile width and left the rest empty; this keeps the icon at its
 * natural size on the standard cover glass, so the tile sits at the same
 * lightness as every other app's cover.
 */
CoverBackground {
    id: cover

    Item {
        anchors {
            fill: parent
            bottomMargin: cover.coverActionArea.height
        }

        Column {
            anchors.centerIn: parent
            width: parent.width
            spacing: Theme.paddingLarge

            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                source: "file:///usr/share/atlantic-browser/data/icon-launcher-browser.png"
                sourceSize.width: Theme.iconSizeLauncher
                sourceSize.height: Theme.iconSizeLauncher
                width: Theme.iconSizeLauncher
                height: Theme.iconSizeLauncher
                fillMode: Image.PreserveAspectFit
                smooth: true
            }

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width - Theme.paddingLarge * 2
                //: Create a new tab cover text
                //% "Create a new tab"
                text: qsTrId("sailfish_browser-he-create_new_tab")
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryColor
            }
        }
    }
}
