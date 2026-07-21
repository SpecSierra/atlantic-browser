/*
 * Copyright (c) 2021 Open Mobile Platform LLC.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

import QtQuick 2.2
import Sailfish.Silica 1.0
import Nemo.DBus 2.0

Item {
    id: root

    property int horizontalOffset
    property int iconWidth
    readonly property int verticalPadding: 3 * Theme.paddingSmall

    height: content.height + verticalPadding * 2

    Column {
        id: content

        width: parent.width
        spacing: Theme.paddingLarge
        y: verticalPadding

        Column {
            width: parent.width

            OverlayListItem {
                height: Theme.itemSizeSmall
                iconWidth: root.iconWidth
                horizontalOffset: root.horizontalOffset
                iconSource: "image://theme/icon-m-tab-new"
                //% "New tab"
                text: qsTrId("sailfish_browser-la-new_tab")
                onClicked: {
                    webView.privateMode = false
                    overlay.toolBar.enterNewTabUrl()
                }
            }

            OverlayListItem {
                height: Theme.itemSizeSmall
                iconWidth: root.iconWidth
                horizontalOffset: root.horizontalOffset
                iconSource: "image://theme/icon-m-incognito-new"
                //% "New private tab"
                text: qsTrId("sailfish_browser-la-new_private_tab")
                onClicked: {
                    webView.privateMode = true
                    overlay.toolBar.enterNewTabUrl()
                }
            }
        }

        Column {
            width: parent.width

            OverlayListItem {
                height: Theme.itemSizeSmall
                iconWidth: root.iconWidth
                horizontalOffset: root.horizontalOffset
                iconSource: "image://theme/icon-m-search-on-page"
                enabled: webView.contentItem
                //% "Search on page"
                text: qsTrId("sailfish_browser-la-search_on_page")

                onClicked: {
                    overlay.toolBar.findInPageActive = true
                    overlay.toolBar.findInPage()
                }
            }

            OverlayListItem {
                height: Theme.itemSizeSmall
                enabled: webView.contentItem
                opacity: enabled ? 1.0 : 0.5
                iconWidth: root.iconWidth
                horizontalOffset: root.horizontalOffset
                iconSource: "image://theme/icon-m-share"
                //% "Share"
                text: qsTrId("sailfish_browser-la-share")

                onClicked: {
                    overlay.toolBar.shareActivePage()
                    overlay.animator.showChrome()
                }
            }

            OverlayListItem {
                height: Theme.itemSizeSmall
                enabled: webView.contentItem
                opacity: enabled ? 1.0 : 0.5
                iconWidth: root.iconWidth
                horizontalOffset: root.horizontalOffset
                iconSource: "image://theme/icon-m-computer"
                checkable: true
                checked: webView.contentItem ? webView.contentItem.desktopMode : false
                //: Label for the toggle that reloads the page as its desktop variant
                //% "Desktop site"
                text: qsTrId("settings_browser-la-desktop_site")

                onClicked: {
                    // setDesktopMode() swaps the UA (atlanticUserAgent) and
                    // reloads so the server sends the matching page variant.
                    webView.contentItem.desktopMode = !webView.contentItem.desktopMode
                    overlay.animator.showChrome()
                }
            }

            OverlayListItem {
                // Host of the current page ("" on about:/file: pages).
                readonly property string _host: {
                    var m = /^https?:\/\/([^\/:?#]+)/.exec(webView.url || "")
                    return m ? m[1].toLowerCase() : ""
                }

                height: Theme.itemSizeSmall
                enabled: _host.length > 0 && adBlockEngine.value
                opacity: enabled ? 1.0 : 0.5
                iconWidth: root.iconWidth
                horizontalOffset: root.horizontalOffset
                // Bundled shield-with-slash glyph (the SFOS theme has no
                // ad-block/shield icon). Monochrome PNG (rasterized from an SVG;
                // the target Qt has no qsvg imageformat plugin): Silica's Icon
                // tints it to Theme.primaryColor/highlight via its alpha, like a
                // theme icon. 96px to stay crisp at the device pixel ratio.
                iconSource: Qt.resolvedUrl("icon-m-adblock.png")
                checkable: true
                // adBlockAllowlist and adBlockEngine resolve from BrowserPage's
                // context (like webView/overlay above).
                checked: enabled && !adBlockAllowlist.isAllowed(_host)
                //: Per-site toggle: unchecking disables the ad blocker on this site
                //% "Adblock"
                text: qsTrId("sailfish_browser-la-block_ads_on_site")

                onClicked: {
                    adBlockAllowlist.setAllowed(_host, checked)
                    overlay.animator.showChrome()
                }
            }

            OverlayListItem {
                // Host of the current page ("" on about:/file: pages).
                readonly property string _host: {
                    var m = /^https?:\/\/([^\/:?#]+)/.exec(webView.url || "")
                    return m ? m[1].toLowerCase() : ""
                }

                height: Theme.itemSizeSmall
                enabled: _host.length > 0
                opacity: enabled ? 1.0 : 0.5
                iconWidth: root.iconWidth
                horizontalOffset: root.horizontalOffset
                iconSource: "image://theme/icon-m-browser-javascript"
                checkable: true
                // javascriptBlocklist resolves from BrowserPage's context (like
                // adBlockAllowlist above). JS is on by default, so checked means
                // enabled = host is NOT on the blocklist.
                checked: enabled && !javascriptBlocklist.isBlocked(_host)
                //: Per-site toggle: unchecking disables JavaScript on this site
                //% "JavaScript"
                text: qsTrId("sailfish_browser-la-javascript_on_site")

                onClicked: {
                    // The Switch has automaticCheck:false, so `checked` still
                    // holds the pre-tap state here (JS currently enabled). New
                    // blocked state = toggle of current = `checked` (enabled ⇒
                    // now block; disabled ⇒ now unblock) — same trick as the
                    // "Block ads" item above.
                    javascriptBlocklist.setBlocked(_host, checked)
                    overlay.animator.showChrome()
                }
            }

        }

        Column {
            width: parent.width

            OverlayListItem {
                height: Theme.itemSizeSmall
                iconWidth: root.iconWidth
                horizontalOffset: root.horizontalOffset
                iconSource: "image://theme/icon-m-favorite-selected"
                //% "Bookmarks"
                text: qsTrId("sailfish_browser-la-bookmarks")

                onClicked: {
                    overlay.animator.showChrome()
                    pageStack.push("../BookmarkPage.qml", { bookmarkModel: overlay.bookmarkModel })
                }
            }

            OverlayListItem {
                height: Theme.itemSizeSmall
                iconWidth: root.iconWidth
                horizontalOffset: root.horizontalOffset
                //% "History"
                text: qsTrId("sailfish_browser-la-history")
                iconSource: "image://theme/icon-m-history"

                onClicked: {
                    overlay.animator.showChrome()
                    var bookmarkModel = overlay.bookmarkModel
                    var historyPage = pageStack.push("../HistoryPage.qml", { model: overlay.historyModel })
                    historyPage.loadPage.connect(overlay.toolBar.loadPage)
                    historyPage.saveBookmark.connect(function(url, title, favicon) {
                        bookmarkModel.add(url, title || url, favicon, true)
                    })
                }
            }

            OverlayListItem {
                height: Theme.itemSizeSmall
                iconWidth: root.iconWidth
                horizontalOffset: root.horizontalOffset
                //% "Downloads"
                text: qsTrId("sailfish_browser-la-downloads")
                iconSource: "image://theme/icon-m-downloads"
                onClicked: {
                    overlay.animator.showChrome()
                    settingsApp.call("showTransfers", [])
                }
            }

            OverlayListItem {
                height: Theme.itemSizeSmall
                iconWidth: root.iconWidth
                horizontalOffset: root.horizontalOffset
                //% "Settings"
                text: qsTrId("sailfish_browser-la-setting")
                iconSource: "image://theme/icon-m-setting"

                onClicked: {
                    overlay.animator.showChrome()
                    pageStack.push(Qt.resolvedUrl("../SettingsPage.qml"))
                }
            }
        }
    }

    DBusInterface {
        id: settingsApp

        service: "com.jolla.settings"
        iface: "com.jolla.settings.ui"
        path: "/com/jolla/settings/ui"
    }
}
