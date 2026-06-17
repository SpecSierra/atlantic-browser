/****************************************************************************
**
** Copyright (c) 2014 Jolla Ltd.
** Contact: Vesa-Matti Hartikainen <vesa-matti.hartikainen@jolla.com>
** Contact: Raine Makelelainen <raine.makelainen@jolla.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.0
import Sailfish.Silica 1.0

Image {
    property string icon

    sourceSize.width: Theme.iconSizeLauncher
    sourceSize.height: Theme.iconSizeLauncher
    width: Theme.iconSizeLauncher
    height: Theme.iconSizeLauncher

    source: {
        if (!icon.length) {
            return "image://theme/icon-launcher-bookmark"
        } else if (icon.indexOf(':/') !== -1
                   || icon.indexOf("data:image/png;base64") === 0
                   || icon.indexOf("data:image/jpeg;base64") === 0) {
            return icon
        } else if (icon.indexOf("iVBOR") === 0) {
            // Raw base64 PNG (page thumbnails stored without a data: URI prefix).
            // Heal legacy entries instead of feeding them to image://theme/.
            return "data:image/png;base64," + icon
        } else if (icon.indexOf("/9j/") === 0) {
            // Raw base64 JPEG stored without a data: URI prefix.
            return "data:image/jpeg;base64," + icon
        } else if (icon.indexOf('/') === 0) {
            return 'file://' + icon.split("/").map(encodeURIComponent).join("/")
        } else {
            return 'image://theme/' + icon
        }
    }

    onStatusChanged: {
        if (status === Image.Error) {
            icon = "icon-launcher-bookmark"
        }
    }
}
