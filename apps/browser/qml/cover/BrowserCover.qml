/****************************************************************************
**
** Copyright (c) 2026 Atlantic Browser
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import QtGraphicalEffects 1.0
import Sailfish.Silica 1.0
import Sailfish.Browser 1.0

/*
 * Cover for "there are tabs open": the active tab's thumbnail fills the top of
 * the tile below the site's favicon and domain, which sit at the top of the
 * card on the cover glass as plain cover text — no bar laid over the picture. A second
 * line carries the tab count and a badge for a tab making noise; loading is
 * shown by a hairline and by the domain taking the highlight colour.
 *
 * The cover window is created by Silica from ApplicationWindow's context, so
 * nothing in this file can reach the browser's ids. Everything comes in
 * through the webView property, which browser.qml binds when it instantiates
 * the cover component.
 */
CoverBackground {
    id: cover

    // WPEWebContainer. Null while the browser is tearing down.
    property QtObject webView

    readonly property QtObject tabModel: webView ? webView.tabModel : null
    readonly property bool privateMode: webView !== null && webView.privateMode
    readonly property string pageUrl: webView ? webView.url : ""
    readonly property string pageTitle: webView ? webView.title : ""
    readonly property int tabCount: tabModel ? tabModel.count : 0
    readonly property bool loading: webView !== null && webView.loading
    readonly property bool audioActive: webView !== null && webView.contentItem !== null
                                        && webView.contentItem.mediaAudioActive

    // Private tabs must never paint page pixels onto the home screen.
    readonly property bool showThumbnail: !privateMode

    property string _favicon: ""
    property Image _activeThumbnail: null

    readonly property bool thumbnailShown: _activeThumbnail !== null
                                           && _activeThumbnail.visible

    function _refreshFavicon() {
        _favicon = (!privateMode && pageUrl != "")
                ? FaviconManager.get("history", pageUrl) : ""
    }

    onPageUrlChanged: _refreshFavicon()
    onPrivateModeChanged: _refreshFavicon()
    Component.onCompleted: _refreshFavicon()

    Connections {
        target: FaviconManager
        // Icons are learned asynchronously, long after the page loaded.
        onIconChanged: cover._refreshFavicon()
    }

    // Everything sits above the cover action area, which lipstick draws over
    // the bottom of the tile.
    Item {
        id: content

        anchors {
            fill: parent
            // Stop exactly where the action band starts. The icons are drawn
            // by lipstick, not by this cover, so their position is fixed and
            // the band's full height is what centres them: taking any of it
            // back for the picture only crowds them from above.
            bottomMargin: cover.coverActionArea.height
        }
        clip: true

        // The site is named at the top of the card, on the cover glass like
        // any other Sailfish cover's text, and the picture fills everything
        // below it — no bar laid over the picture.
        Item {
            id: thumbnailArea

            anchors {
                left: parent.left
                right: parent.right
                top: pageInfo.bottom
                // A little air between the text and the picture's edge.
                topMargin: Theme.paddingSmall
                bottom: parent.bottom
            }

            // Rounded on every corner, so the page reads as inset into the
            // card rather than as a photo bleeding off its edges.
            layer.enabled: cover.thumbnailShown
            layer.effect: OpacityMask {
                maskSource: thumbnailMask
            }

            // Active tab's thumbnail. The model is walked with a Repeater
            // because QML has no way to index a QAbstractListModel directly;
            // only the active row ever loads an image.
            Repeater {
                model: cover.showThumbnail ? cover.tabModel : null

                delegate: Image {
                    id: thumbnail

                    anchors.fill: parent
                    source: model.activeTab ? model.thumbnailPath : ""
                    visible: model.activeTab && status === Image.Ready
                    cache: false
                    asynchronous: true
                    fillMode: Image.PreserveAspectCrop
                    verticalAlignment: Image.AlignTop
                    opacity: visible ? 1.0 : 0.0
                    Behavior on opacity { FadeAnimation {} }

                    // Let the cover reach the active row's image without
                    // walking the delegates: the binding is dropped again as
                    // soon as this row stops being the active tab.
                    Binding {
                        target: cover
                        property: "_activeThumbnail"
                        value: thumbnail
                        when: model.activeTab
                    }
                }
            }

            // Private mode says what it is instead of showing the page.
            Column {
                anchors.centerIn: parent
                width: parent.width
                spacing: Theme.paddingMedium
                visible: cover.privateMode

                Image {
                    anchors.horizontalCenter: parent.horizontalCenter
                    source: "image://theme/icon-m-incognito?" + Theme.primaryColor
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTrId("sailfish_browser-la-private_mode")
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.secondaryColor
                }
            }

            // Nothing captured yet (tab restored but not painted, or the grab
            // has not landed): show the page's title rather than empty glass.
            Label {
                anchors {
                    left: parent.left
                    right: parent.right
                    verticalCenter: parent.verticalCenter
                    margins: Theme.paddingMedium
                }
                visible: !cover.privateMode && !cover.thumbnailShown && text != ""
                text: cover.pageTitle
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.primaryColor
                wrapMode: Text.Wrap
                maximumLineCount: 4
                elide: Text.ElideRight
            }
        }

        Rectangle {
            id: thumbnailMask

            anchors.fill: thumbnailArea
            visible: false
            radius: Theme.paddingMedium
        }

        // Load progress of the tab left behind, so the cover tells you when the
        // page you backgrounded has finished.
        Rectangle {
            anchors.top: thumbnailArea.top
            width: parent.width * (webView ? webView.loadProgress / 100.0 : 0)
            height: Math.round(Theme.paddingSmall / 2)
            color: Theme.highlightColor
            opacity: cover.loading ? 1.0 : 0.0
            Behavior on opacity { FadeAnimation {} }
            Behavior on width { NumberAnimation { duration: 200; easing.type: Easing.InOutQuad } }
        }

        // Cover text: the site on one line, what else is open on the next.
        Column {
            id: pageInfo

            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
                leftMargin: Theme.paddingMedium
                rightMargin: Theme.paddingMedium
                topMargin: Theme.paddingSmall
            }
            spacing: Theme.paddingSmall / 2

            // Anchored rather than laid out in a Row: inside a Row the label's
            // width binding did not constrain it and the domain ran off the
            // tile, hard-clipped by the cover edge instead of fading.
            Item {
                width: parent.width
                height: Math.max(faviconImage.height, domainLabel.implicitHeight)

                Image {
                    id: faviconImage

                    anchors {
                        left: parent.left
                        verticalCenter: parent.verticalCenter
                    }
                    // Sized to the line of text it sits on, so the strip stays
                    // as slim as the type rather than as tall as an icon.
                    width: Math.round(Theme.iconSizeExtraSmall * 0.6)
                    height: width
                    sourceSize.width: Theme.iconSizeExtraSmall
                    sourceSize.height: Theme.iconSizeExtraSmall
                    asynchronous: true
                    fillMode: Image.PreserveAspectFit
                    visible: !cover.privateMode && status === Image.Ready
                    // Learned favicons are stored either as a data: URI, a raw
                    // base64 payload or a theme icon name, exactly as
                    // FavoriteIcon resolves them.
                    source: {
                        var icon = cover._favicon
                        if (!icon.length) {
                            return ""
                        } else if (icon.indexOf(':/') !== -1
                                   || icon.indexOf("data:image/png;base64") === 0
                                   || icon.indexOf("data:image/jpeg;base64") === 0) {
                            return icon
                        } else if (icon.indexOf("iVBOR") === 0) {
                            return "data:image/png;base64," + icon
                        } else if (icon.indexOf("/9j/") === 0) {
                            return "data:image/jpeg;base64," + icon
                        } else if (icon.indexOf('/') === 0) {
                            return 'file://' + icon.split("/").map(encodeURIComponent).join("/")
                        } else {
                            return 'image://theme/' + icon
                        }
                    }
                }

                Label {
                    id: domainLabel

                    anchors {
                        left: faviconImage.visible ? faviconImage.right : parent.left
                        leftMargin: faviconImage.visible ? Theme.paddingSmall : 0
                        right: parent.right
                        verticalCenter: parent.verticalCenter
                    }
                    text: cover.privateMode
                          ? "" : WebUtils.displayableUrl(cover.pageUrl)
                    // A cover is narrow and hosts are long: shrink the text to
                    // fit rather than truncate it, the way CoverPlaceholder
                    // fits its text vertically. Capped at tiny — with
                    // HorizontalFit a long host is sized by the tile width, so
                    // a larger cap only makes short hosts shout.
                    font.pixelSize: Theme.fontSizeTiny
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: Math.round(Theme.fontSizeTiny * 0.75)
                    elide: Text.ElideRight
                    // Loading is told by the colour rather than by more chrome.
                    color: cover.loading ? Theme.highlightColor : Theme.primaryColor
                }
            }

            Row {
                width: parent.width
                spacing: Theme.paddingSmall
                visible: cover.tabCount > 1 || cover.audioActive

                Image {
                    anchors.verticalCenter: parent.verticalCenter
                    source: "image://theme/icon-m-browser-sound?" + Theme.secondaryColor
                    sourceSize.width: Theme.iconSizeExtraSmall
                    sourceSize.height: Theme.iconSizeExtraSmall
                    width: Math.round(Theme.iconSizeExtraSmall * 0.7)
                    height: width
                    visible: cover.audioActive
                }

                Image {
                    anchors.verticalCenter: parent.verticalCenter
                    source: "image://theme/icon-m-tabs?" + Theme.secondaryColor
                    sourceSize.width: Theme.iconSizeExtraSmall
                    sourceSize.height: Theme.iconSizeExtraSmall
                    width: Math.round(Theme.iconSizeExtraSmall * 0.7)
                    height: width
                    visible: cover.tabCount > 1
                }

                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: cover.tabCount > 1 ? cover.tabCount : ""
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.secondaryColor
                }
            }
        }
    }
}
