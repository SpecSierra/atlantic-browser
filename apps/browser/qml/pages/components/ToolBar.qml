/*
 * Copyright (c) 2014 - 2019 Jolla Ltd.
 * Copyright (c) 2019 - 2021 Open Mobile Platform LLC.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

import QtQuick 2.2
import Sailfish.Silica 1.0
import Sailfish.Browser 1.0
import "." as Browser
import "../../shared" as Shared

Column {
    id: toolBarRow

    property string url
    property string findText
    property real secondaryToolsHeight
    property bool secondaryToolsActive
    property bool findInPageActive
    property real certOverlayHeight
    property bool certOverlayActive
    property real certOverlayAnimPos
    property real certOverlayPreferedHeight: 4 * toolBarRow.height
    readonly property bool showFindButtons: findInPageActive && webView.contentItem && webView.contentItem.findInPageHasResult
    property var bookmarked
    readonly property alias rowHeight: toolsRow.height
    readonly property int maxRowCount: 6

    readonly property int horizontalOffset: largeScreen ? Theme.paddingLarge : Theme.paddingSmall
    readonly property int buttonPadding: largeScreen
                                         || orientation === Orientation.Landscape
                                         || orientation === Orientation.LandscapeInverted
                                         ? Theme.paddingMedium : Theme.paddingSmall
    readonly property int iconWidth: largeScreen ? (Theme.iconSizeLarge + 3 * buttonPadding)
                                                 : (Theme.iconSizeMedium + 2 * buttonPadding)
    readonly property int smallIconWidth: largeScreen ? (Theme.iconSizeMedium + 3 * buttonPadding)
                                                      : (Theme.iconSizeSmall + 2 * buttonPadding)
    // Height of toolbar should be such that viewport height is
    // even number both chrome and fullscreen modes. For instance
    // height of 110px for toolbar would result 1px rounding
    // error in chrome mode as viewport height would be 850px. This would
    // result in CSS pixels viewport height of 566.66..px -> rounded to 566px.
    // So, we make sure below that (device height - toolbar height) / pixel ratio is even number.
    // target values when Theme.pixelratio == 1 are:
    // portrait: 108px
    // landcape: 78px
    property int scaledPortraitHeight: Screen.height
                                       - Math.floor((Screen.height - Settings.toolbarLarge * Theme.pixelRatio)
                                                    / WebUtils.cssPixelRatio)
                                       * WebUtils.cssPixelRatio
    property int scaledLandscapeHeight: Screen.width
                                        - Math.floor((Screen.width - Settings.toolbarSmall * Theme.pixelRatio)
                                                     / WebUtils.cssPixelRatio)
                                        * WebUtils.cssPixelRatio

    signal showTabs
    signal showOverlay
    signal showSecondaryTools
    signal showInfoOverlay
    signal showChrome
    signal showCertDetail

    // Used from the PopUpMenu
    signal loadPage(string url)
    signal enterNewTabUrl
    signal findInPage
    signal shareActivePage
    signal bookmarkActivePage
    signal removeActivePageFromBookmarks
    function resetFind() {
        if (webView.contentItem) webView.contentItem.findFinish()
        if (webView.contentItem) {
            webView.contentItem.forceChrome(false)
        }

        findInPageActive = false
    }

    width: parent.width

    onFindInPageActiveChanged: {
        // Down allow hiding of toolbar when finding text from the page.
        if (findInPageActive && webView.contentItem) {
            webView.contentItem.forceChrome(true)
        }
    }

    Item {
        id: certOverlay

        readonly property bool canDestroy: !visible && !certOverlayActive
        onCanDestroyChanged: {
            if (canDestroy) certOverlayLoader.active = false
        }

        visible: opacity > 0.0 || height > 0.0
        opacity: certOverlayActive ? 1.0 : 0.0
        height: certOverlayHeight
        width: parent.width

        Behavior on opacity { FadeAnimation {} }

        Loader {
            id: certOverlayLoader

            active: false
            sourceComponent: CertificateInfo {
                security: webView.contentItem ? webView.contentItem.security : null
                width: certOverlay.width
                height: certOverlayHeight
                opacity: Math.max((certOverlayAnimPos * 2.0) - 1.0, 0)

                onShowCertDetail: toolBarRow.showCertDetail()
                onCloseCertInfo: showChrome()

                onContentHeightChanged: {
                    if (contentHeight != 0) {
                        certOverlayPreferedHeight = contentHeight
                    }
                }
            }
            onLoaded: toolBarRow.showInfoOverlay()
        }
    }

    // Horizontal swipe on the toolbar navigates history: swipe right = back,
    // swipe left = forward. drag.filterChildren keeps the buttons fully
    // functional — taps and vertical overlay drags pass through, the swipe
    // only steals the grab once the finger travels horizontally. The row
    // follows the finger and springs back on release; an arrow fades in at
    // the edge and highlights once the swipe is past the trigger distance.
    MouseArea {
        id: navSwipeArea

        readonly property real triggerDistance: width / 6
        readonly property bool backArmed: navDragTarget.x > triggerDistance && webView.canGoBack
        readonly property bool forwardArmed: navDragTarget.x < -triggerDistance && webView.canGoForward

        width: parent.width
        height: toolsRow.height
        enabled: overlayAnimator.atBottom && !findInPageActive
                 && !certOverlayActive && !secondaryToolsActive

        drag.target: navDragTarget
        drag.axis: Drag.XAxis
        drag.filterChildren: true
        // Keep a short leash in directions with no history to go to.
        drag.minimumX: webView.canGoForward ? -width / 3 : -Theme.paddingLarge
        drag.maximumX: webView.canGoBack ? width / 3 : Theme.paddingLarge

        drag.onActiveChanged: {
            if (drag.active) {
                return
            }
            if (backArmed) {
                webView.goBack()
            } else if (forwardArmed) {
                webView.goForward()
            }
            navDragTarget.x = 0
        }

        // The auto-hide timer only sees touches on the web view; keep the
        // toolbar from vanishing under the user's finger mid-swipe.
        onPressed: chromeAutoHideTimer.restart()

        Item {
            id: navDragTarget

            Behavior on x {
                enabled: !navSwipeArea.drag.active
                NumberAnimation { duration: 200; easing.type: Easing.OutQuad }
            }
        }

        Image {
            anchors {
                left: parent.left
                leftMargin: Theme.paddingLarge
                verticalCenter: parent.verticalCenter
            }
            source: "image://theme/icon-m-back?"
                    + (navSwipeArea.backArmed ? Theme.highlightColor : Theme.primaryColor)
            opacity: webView.canGoBack ? Math.min(navDragTarget.x / navSwipeArea.triggerDistance, 1.0) : 0.0
        }

        Image {
            anchors {
                right: parent.right
                rightMargin: Theme.paddingLarge
                verticalCenter: parent.verticalCenter
            }
            source: "image://theme/icon-m-forward?"
                    + (navSwipeArea.forwardArmed ? Theme.highlightColor : Theme.primaryColor)
            opacity: webView.canGoForward ? Math.min(-navDragTarget.x / navSwipeArea.triggerDistance, 1.0) : 0.0
        }


        Row {
            id: toolsRow

            property int biggestCorner: Math.max(Screen.topLeftCorner.radius,
                                                 Screen.topRightCorner.radius,
                                                 Screen.bottomLeftCorner.radius,
                                                 Screen.bottomRightCorner.radius)
            // items have some padding of their own so don't mind a little overlap with the rounding
            x: Math.round(biggestCorner * 0.4)
            width: parent.width - 2*x
            height: browserPage.isPortrait ? scaledPortraitHeight : scaledLandscapeHeight

            transform: Translate { x: navDragTarget.x }

            Shared.ExpandingButton {
                id: backIcon

                readonly property bool isHome: !webView.canGoBack
                                               && !(webView.contentItem && webView.contentItem.parentId > 0)
                                               && !!webView.contentItem

                height: parent.height
                expandedWidth: toolBarRow.iconWidth
                icon {
                    // Native home glyph is a wide house; render it a bit narrower.
                    width: backIcon.isHome ? Math.round(Theme.iconSizeMedium * 0.9) : Theme.iconSizeMedium
                    height: Theme.iconSizeMedium
                    source: {
                        if (webView.canGoBack) {
                            return "image://theme/icon-m-back"
                        } else if (webView.contentItem && webView.contentItem.parentId > 0) {
                            return "image://theme/icon-m-back-tab"
                        } else if (webView.contentItem) {
                            return "image://theme/icon-m-home"
                        }
                        return ""
                    }

                    onStatusChanged: {
                        // Use icon-m-back as a fallback. The icon-m-back-tab
                        // is a new icon and may not exist.
                        if (icon.status == Image.Error && icon.source == "image://theme/icon-m-back-tab") {
                            icon.source = "image://theme/icon-m-back"
                        }
                    }
                }

                active: (webView.canGoBack || (webView.contentItem && webView.contentItem.parentId > 0)
                         || webView.contentItem) && !findInPageActive
                onTapped: {
                    if (webView.canGoBack) {
                        webView.goBack()
                    } else if (webView.contentItem && webView.contentItem.parentId > 0) {
                        webView.tabModel.closeActiveTab()
                    } else {
                        toolBarRow.loadPage(WebUtils.homePage)
                    }
                }
            }

            Item {
                id: padlockIcon

                readonly property var sec: webView.contentItem ? webView.contentItem.security : null
                // https with certificate errors (self-signed, expired, bad identity, ...)
                readonly property bool danger: sec && sec.validState && !sec.allGood
                // plain http: no TLS at all
                readonly property bool insecure: !webView.loading && !(sec && sec.validState)
                        && webView.url.toString().indexOf("http://") === 0
                readonly property bool active: ((sec && sec.validState) || insecure) && !findInPageActive
                        && !(webView.url.indexOf("about:") === 0)
                        && (!webView.contentItem || !webView.contentItem.textSelectionActive)
                property real glow

                height: parent.height
                width: toolBarRow.smallIconWidth

                Shared.IconButton {
                    anchors.fill: parent
                    opacity: padlockIcon.active ? 1.0 : 0.0
                    Behavior on opacity { FadeAnimation {} }

                    icon.source: padlockIcon.danger ? "image://theme/icon-s-filled-warning"
                                 : padlockIcon.insecure ? "image://theme/icon-s-warning"
                                                        : "image://theme/icon-s-outline-secure"
                    icon.color: (padlockIcon.danger || padlockIcon.insecure)
                        ? Qt.tint(Theme.primaryColor,
                                  Qt.rgba(Theme.errorColor.r, Theme.errorColor.g,
                                          Theme.errorColor.b, padlockIcon.glow))
                        : Theme.primaryColor
                    enabled: !!padlockIcon.sec && padlockIcon.active
                    onTapped: {
                        if (certOverlayActive) {
                            showChrome()
                        } else {
                            certOverlayLoader.active = true
                        }
                    }
                }

                SequentialAnimation {
                    id: securityAnimation

                    PauseAnimation { duration: 2000 }
                    NumberAnimation {
                        target: padlockIcon; property: "glow";
                        to: 0.0; duration: 1000; easing.type: Easing.OutCubic
                    }
                }

                function warn() {
                    glow = 1.0
                    securityAnimation.start()
                }

                onDangerChanged: warn()
                onInsecureChanged: if (insecure) warn()

                Connections {
                    target: webView
                    onLoadingChanged: {
                        if (!webView.loading && (padlockIcon.danger || padlockIcon.insecure)) {
                            padlockIcon.warn()
                        }
                    }
                }
            }

            BackgroundItem {
                id: touchArea

                readonly property bool down: pressed && containsMouse

                height: parent.height
                width: toolsRow.width - (tabButton.width + stopButton.width + padlockIcon.width + backIcon.width + menuButton.width)
                enabled: !showFindButtons
                _showPress: false

                onClicked: {
                    if (findInPageActive) {
                        findInPage()
                    } else {
                        toolBarRow.showOverlay()
                    }
                }

                onPressAndHold: {
                    var url = webView.url
                    if (url) {
                        // encode the string if it looks like it has query or fragment parts
                        // FIXME: could be improved with *proper* matching.
                        Clipboard.text = ( (url.indexOf('?') > -1) || (url.indexOf('#') > -1) ) ? encodeURI(url) : url
                        urlCopyNotice.show()
                    }
                }

                Notice {
                    id: urlCopyNotice

                    duration: Notice.Short
                    verticalOffset: -Theme.itemSizeMedium
                    //: Url copied to clipboard from toolbar (long press).
                    //% "Url copied to clipboard"
                    text: qsTrId("sailfish_browser-la-url_copied_to_clipboard")
                }

                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width + Theme.paddingMedium
                    color: touchArea.highlighted ? Theme.highlightColor : Theme.primaryColor

                    text: {
                        if (findInPageActive) {
                            //: No text search results were found from the page.
                            //% "No results"
                            return qsTrId("sailfish_browser-la-no_results")
                        } else if (url == "about:blank" || (webView.completed && webView.tabModel.count === 0)) {
                            //: Placeholder text for url typing and searching
                            //% "Type URL or search"
                            return qsTrId("sailfish_browser-ph-type_url_or_search")
                        } else if (url) {
                            return WebUtils.displayableUrl(url)
                        } else {
                            //: Loading text that is visible when url is not yet resolved.
                            //% "Loading"
                            return qsTrId("sailfish_browser-la-loading")
                        }
                    }

                    truncationMode: TruncationMode.Fade

                    // Hide the address while the popup menu is open — it would otherwise
                    // show at the bottom, under/beside the menu.
                    opacity: (showFindButtons || secondaryToolsActive) ? 0.0 : 1.0
                    Behavior on opacity { FadeAnimation {} }
                }

                Shared.ExpandingButton {
                    id: previousFindResult

                    active: showFindButtons
                    height: parent.height
                    expandedWidth: (toolsRow.width - menuButton.width - tabButton.width) / 2
                    icon {
                        source: "image://theme/icon-m-left"
                        anchors.horizontalCenterOffset: Theme.paddingLarge
                    }

                    onTapped: {
                        if (webView.contentItem) webView.contentItem.findText(findText, true)
                    }
                }

                Shared.ExpandingButton {
                    active: showFindButtons
                    height: parent.height
                    expandedWidth: previousFindResult.width
                    anchors.left: previousFindResult.right
                    icon {
                        source: "image://theme/icon-m-right"
                        anchors.horizontalCenterOffset: -Theme.paddingLarge
                    }

                    onTapped: {
                        if (webView.contentItem) webView.contentItem.findText(findText, false)
                    }
                }
            }

            Shared.ExpandingButton {
                id: stopButton

                height: parent.height
                expandedWidth: toolBarRow.iconWidth
                icon.source: webView.loading ? "image://theme/icon-m-reset" : "image://theme/icon-m-refresh"
                active: webView.contentItem && !findInPageActive

                Behavior on opacity { FadeAnimation {} }

                onTapped: {
                    if (webView.loading) {
                        webView.stop()
                    } else {
                        webView.reload()
                    }
                    toolBarRow.showChrome()
                }
            }

            // Container item for cross fading tabs, close, find in page button (and keep Row's width still).
            Item {
                id: tabButton

                width: toolBarRow.iconWidth + toolBarRow.horizontalOffset
                height: parent.height

                Browser.TabButton {
                    id: tabs

                    width: parent.width
                    height: parent.height
                    icon.source: {
                        if (webView.privateMode) {
                            return webView.tabModel.count > 0 ? "image://theme/icon-m-incognito-selected"
                                                              : "image://theme/icon-m-incognito"
                        }

                        return "image://theme/icon-m-tabs"
                    }

                    label.color: {
                        if (webView.privateMode) {
                            return Theme.overlayBackgroundColor ? Theme.overlayBackgroundColor : "black"
                        }

                        return highlighted ? Theme.highlightColor : Theme.primaryColor
                    }

                    opacity: findInPageActive ? 0.0 : 1.0
                    horizontalOffset: -toolBarRow.horizontalOffset
                    label.text: webView.privateMode && (webView.tabModel.count === 0) ? "" : webView.tabModel.count
                    onTapped: toolBarRow.showTabs()

                    RotationAnimator {
                        id: rotationAnimator

                        target: tabs.icon
                        duration: 1500
                        alwaysRunToEnd: true
                    }

                    Connections {
                        target: webView.tabModel
                        onNewTabRequested: {
                            rotationAnimator.from = 0
                            rotationAnimator.to = 360
                            rotationAnimator.restart()
                        }

                        onTabClosed: {
                            rotationAnimator.from = 0
                            rotationAnimator.to = -360
                            rotationAnimator.restart()
                        }
                    }
                }

                Shared.IconButton {
                    width: parent.width
                    height: parent.height
                    opacity: !secondaryToolsActive && findInPageActive ? 1.0 : 0.0
                    icon.source: "image://theme/icon-m-search"
                    icon.anchors.horizontalCenterOffset: -toolBarRow.horizontalOffset
                    onTapped: {
                        findInPageActive = true
                        findInPage()
                    }
                }
            }

            Item {
                id: menuButton

                width: toolBarRow.iconWidth + toolBarRow.horizontalOffset
                height: parent.height

                Shared.IconButton {
                    icon.source: "image://theme/icon-m-menu"
                    icon.anchors.horizontalCenterOffset: - toolBarRow.horizontalOffset
                    height: parent.height
                    width: parent.width
                    opacity: findInPageActive ? 0.0 : 1.0
                    onTapped: showSecondaryTools()
                }

                Shared.IconButton {
                    icon.source: "image://theme/icon-m-reset"
                    icon.anchors.horizontalCenterOffset: - toolBarRow.horizontalOffset
                    height: parent.height
                    width: parent.width
                    opacity: findInPageActive ? 1.0 : 0.0
                    onTapped: {
                        resetFind()
                        showChrome()
                    }
                }
            }
        }
    }
}
