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

    readonly property real overlayOpacity: 0.15

    // Same derivation as PopUpMenuItem.pageHost: "" when no real page is open.
    readonly property string pageHost: {
        var m = /^https?:\/\/([^\/:?#]+)/.exec(webView.url || "")
        return m ? m[1].toLowerCase() : ""
    }

    radius: 0
    tintAlpha: 0.6

    height: Theme.itemSizeMedium - Theme.paddingMedium
    implicitWidth: content.width
    implicitHeight: Theme.iconSizeMedium

    Row {
        id: content

        height: root.height

        // Duplicates the "New tab" row above on purpose: this strip is the one
        // part of the menu that is always within thumb reach, and opening a tab
        // is the most common reason for coming in here.
        Shared.IconButton {
            height: parent.height
            width: Theme.itemSizeLarge
            icon.source: "image://theme/icon-m-tab-new"
            onTapped: {
                webView.privateMode = false
                overlay.toolBar.enterNewTabUrl()
            }
        }

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
            // There has to be a page to bookmark. webView.contentItem is not
            // that test: it is non-null straight after startup with no tabs
            // open, which left this live on the start page while the per-site
            // toggles above it were correctly greyed out.
            enabled: root.pageHost.length > 0
            onTapped: {
                if (overlay.toolBar.bookmarked) {
                    overlay.toolBar.removeActivePageFromBookmarks()
                } else {
                    overlay.toolBar.bookmarkActivePage()
                }
            }
        }


    }
}
