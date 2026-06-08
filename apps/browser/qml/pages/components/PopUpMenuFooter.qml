/*
 * Copyright (c) 2021 Open Mobile Platform LLC.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

import QtQuick 2.2
import Sailfish.Silica 1.0
import "../../shared" as Shared
import "." as Components

Components.FrostedBox {
    id: root

    property var adBlockConf: null

    readonly property real overlayOpacity: 0.15

    radius: 0
    tintAlpha: 0.6

    height: Theme.itemSizeMedium - Theme.paddingMedium
    implicitWidth: content.width
    implicitHeight: Theme.iconSizeMedium

    Row {
        id: content

        height: root.height

        Shared.IconButton {
            height: parent.height
            width: Theme.itemSizeLarge
            icon.source: "image://theme/icon-m-tab-close"
            icon.opacity: enabled ? 1.0 : Theme.opacityLow
            enabled: webView.tabModel.count > 0
            onTapped: {
                webView.tabModel.closeActiveTab()
                if (webView.tabModel.count === 0) {
                    overlay.startPage(PageStackAction.Animated)
                }
            }
        }

        Shared.IconButton {
            height: parent.height
            width: Theme.itemSizeLarge
            icon.source: "image://theme/icon-m-forward"
            icon.opacity: enabled ? 1.0 : Theme.opacityLow
            enabled: webView.canGoForward
            onTapped: {
                webView.goForward()
                overlay.animator.showChrome()
            }
        }

        Shared.IconButton {
            height: parent.height
            width: Theme.itemSizeLarge
            icon.source: overlay.toolBar.bookmarked ? "image://theme/icon-m-favorite-selected"
                                                    : "image://theme/icon-m-favorite"
            icon.opacity: enabled ? 1.0 : Theme.opacityLow
            enabled: webView.contentItem
            onTapped: {
                if (overlay.toolBar.bookmarked) {
                    overlay.toolBar.removeActivePageFromBookmarks()
                } else {
                    overlay.toolBar.bookmarkActivePage()
                }
            }
        }

        Shared.IconButton {
            height: parent.height
            width: Theme.itemSizeLarge
            icon.source: "image://theme/icon-m-permission"
            icon.opacity: (root.adBlockConf && root.adBlockConf.value) ? 1.0 : Theme.opacityLow
            enabled: webView.contentItem
            onTapped: {
                if (root.adBlockConf) {
                    root.adBlockConf.value = !root.adBlockConf.value
                }
            }
        }


    }
}
