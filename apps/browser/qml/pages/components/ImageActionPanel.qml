/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.6
import Sailfish.Silica 1.0
import Sailfish.Browser 1.0

// Long-press action panel: links, images, and browser.contextMenus entries —
// Atlantic has no menu bar, so a long press is where all of those have to live.
//
// Driven by the bound webView's contentItem PROPERTIES (imageLongPressUrl and
// contextInfo) — NOT a recvAsyncMessage/signal handler, which never fires
// because the WebKit script-message callback runs outside the QML JS context
// (same reason SelectMenuOverlay uses selectMenuActive bindings).
//
// Shaped like SelectMenuOverlay rather than a bare DockedPanel: a square-edged
// sheet that dismisses on a tap outside it, which is how Silica menus behave.
// The backdrop catching that tap is invisible -- dimming the page behind the
// sheet was tried and looked worse. The panel is still a DockedPanel so it
// keeps the slide-up, and it brings its own opaque `background` -- the default
// PanelBackground is a translucent gradient meant to sit over an app's own
// background, and over live web content it just looked like a smudge.
Item {
    id: root

    property var webView
    readonly property var context:
        (webView && webView.contentItem) ? webView.contentItem.contextInfo : ({})
    readonly property string pendingImageUrl:
        (webView && webView.contentItem) ? webView.contentItem.imageLongPressUrl : ""
    // Set for a long press that landed anywhere inside an <a href>, image links
    // included — those offer both the link and the image actions. Restricted to
    // http(s) because that is all "open in new tab" can do anything with; keep
    // in sync with isNavigableLink() in WPEUserScripts.h, which decides whether
    // the press is worth reporting at all.
    readonly property string pendingLinkUrl: {
        var url = (context && context.linkUrl) ? context.linkUrl : ""
        return /^https?:\/\//i.test(url) ? url : ""
    }

    // Re-queried whenever the long press reports a new context, or an extension
    // adds or removes an item.
    property var extensionItems: []

    readonly property bool active:
        pendingImageUrl.length > 0 || pendingLinkUrl.length > 0 || extensionItems.length > 0

    // What the press landed on, for the sheet's heading. The scheme goes: it is
    // never the interesting part and it costs the width the host needs.
    readonly property string targetLabel: {
        var url = pendingLinkUrl.length > 0 ? pendingLinkUrl : pendingImageUrl
        if (url.length === 0)
            return ""
        return url.replace(/^[a-z]+:\/\//i, "")
    }

    function refreshExtensionItems() {
        extensionItems = (context && context.pageUrl)
                ? WebExtensionManager.contextMenuItems(context)
                : []
    }

    function dismiss() {
        if (webView && webView.contentItem) {
            webView.contentItem.clearImageLongPress()
            webView.contentItem.clearContextInfo()
        }
    }

    onContextChanged: refreshExtensionItems()

    Connections {
        target: WebExtensionManager
        onContextMenuItemsChanged: root.refreshExtensionItems()
    }

    anchors.fill: webView
    z: 1000
    // Kept alive through the closing animation, or the sheet would vanish
    // instead of sliding away.
    visible: active || panel.expanded

    // Catches the tap that dismisses the sheet without dimming the page behind
    // it -- same trick as SelectMenuOverlay's invisible backdrop.
    MouseArea {
        anchors.fill: parent
        onClicked: root.dismiss()
    }

    DockedPanel {
        id: panel

        width: parent.width
        height: sheet.height
        dock: Dock.Bottom
        open: root.active

        background: Rectangle {
            color: Theme.colorScheme === Theme.LightOnDark
                   ? Qt.darker(Theme.overlayBackgroundColor, 1.2)
                   : Theme.overlayBackgroundColor
        }

        Column {
            id: sheet

            width: parent.width
            topPadding: Theme.paddingLarge
            bottomPadding: Theme.paddingLarge

            // Heading: the thing you pressed, so the actions below have an
            // obvious subject. Same role as SelectMenuOverlay's "Select option".
            // A URL that does not fit is not truncated -- it scrolls sideways,
            // because the tail of a link (the path, the query) is usually the
            // part you long-pressed it to check. Short ones stay centred.
            Flickable {
                id: targetStrip

                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                height: targetText.height + Theme.paddingMedium
                contentWidth: Math.max(targetText.width, width)
                flickableDirection: Flickable.HorizontalFlick
                clip: true
                visible: root.targetLabel.length > 0

                Label {
                    id: targetText

                    x: width < targetStrip.width ? (targetStrip.width - width) / 2 : 0
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.secondaryHighlightColor
                    text: root.targetLabel
                }

                HorizontalScrollDecorator {}
            }

            Rectangle {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                height: Math.max(1, Math.round(Theme.dp(1)))
                color: Theme.rgba(Theme.primaryColor, 0.15)
                visible: root.targetLabel.length > 0
            }

            // Actions, styled like Silica menu items: centred, one row each, so
            // the sheet reads as a menu instead of a strip of buttons. Buttons
            // in a row also ran out of width once links were added.
            BackgroundItem {
                width: parent.width
                height: Theme.itemSizeSmall
                visible: root.pendingLinkUrl.length > 0

                Label {
                    anchors.centerIn: parent
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    horizontalAlignment: Text.AlignHCenter
                    truncationMode: TruncationMode.Fade
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                    //% "Open in new tab"
                    text: qsTrId("sailfish_browser-me-open_new_tab")
                }

                onClicked: {
                    var url = root.pendingLinkUrl
                    root.dismiss()
                    if (root.webView && url.length > 0) {
                        root.webView.clearSelection()
                        root.webView.load(url, "", true /* newTab */)
                    }
                }
            }

            BackgroundItem {
                width: parent.width
                height: Theme.itemSizeSmall
                visible: root.pendingImageUrl.length > 0

                Label {
                    anchors.centerIn: parent
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    horizontalAlignment: Text.AlignHCenter
                    truncationMode: TruncationMode.Fade
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                    //% "Save image"
                    text: qsTrId("atlantic-bt-save_image")
                }

                onClicked: {
                    if (root.webView.contentItem && root.pendingImageUrl.length > 0)
                        root.webView.contentItem.downloadUrl(root.pendingImageUrl)
                    root.dismiss()
                }
            }

            Repeater {
                model: root.extensionItems

                BackgroundItem {
                    width: sheet.width
                    height: Theme.itemSizeSmall
                    enabled: modelData.enabled

                    Label {
                        anchors.centerIn: parent
                        width: parent.width - 2 * Theme.horizontalPageMargin
                        horizontalAlignment: Text.AlignHCenter
                        truncationMode: TruncationMode.Fade
                        opacity: modelData.enabled ? 1.0 : Theme.opacityLow
                        color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                        text: modelData.type === "checkbox" && modelData.checked
                              ? "✓ " + modelData.title
                              : modelData.title
                    }

                    onClicked: {
                        WebExtensionManager.activateContextMenuItem(
                                    modelData.extensionId, modelData.itemId, root.context)
                        root.dismiss()
                    }
                }
            }
        }
    }
}
