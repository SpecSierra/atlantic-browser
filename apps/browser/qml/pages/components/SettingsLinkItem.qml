/****************************************************************************
**
** Copyright (c) 2026 SpecSierra
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.6
import Sailfish.Silica 1.0

// One "opens another page" row in Settings: icon, label, optional second line.
// These used to be four hand-copied Rows inside BackgroundItems, which is how
// they ended up with different icon sizing and only one of them highlighting
// its label on press.
BackgroundItem {
    id: root

    property alias iconSource: icon.source
    property alias text: label.text
    property string description

    width: parent.width
    contentHeight: Math.max(Theme.itemSizeMedium, content.height + 2 * Theme.paddingMedium)

    Row {
        id: content

        width: parent.width - 2 * Theme.horizontalPageMargin
        x: Theme.horizontalPageMargin
        spacing: Theme.paddingMedium
        anchors.verticalCenter: parent.verticalCenter

        Icon {
            id: icon

            // Sized explicitly: our own extension glyph is an SVG, which would
            // otherwise rasterise at its intrinsic size and sit a few pixels
            // off the theme icons above it.
            sourceSize.width: Theme.iconSizeMedium
            sourceSize.height: Theme.iconSizeMedium
        }

        Column {
            width: parent.width - parent.spacing - icon.width
            anchors.verticalCenter: icon.verticalCenter

            Label {
                id: label

                width: parent.width
                color: root.highlighted ? Theme.highlightColor : Theme.primaryColor
                truncationMode: TruncationMode.Fade
            }

            Label {
                width: parent.width
                visible: text.length > 0
                text: root.description
                font.pixelSize: Theme.fontSizeExtraSmall
                color: root.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                wrapMode: Text.Wrap
            }
        }
    }
}
