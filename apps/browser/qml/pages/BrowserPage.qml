/****************************************************************************
**
** Copyright (c) 2013 - 2021 Jolla Ltd.
** Copyright (c) 2019 - 2021 Open Mobile Platform LLC.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */


import QtQuick 2.2
import QtQuick.Window 2.2 as QuickWindow
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0
import Sailfish.Silica.private 1.0 as Private
import Sailfish.Browser 1.0
import Sailfish.Policy 1.0
import Nemo.Configuration 1.0
import "components" as Browser
import "../shared" as Shared

Page {
    id: browserPage

    readonly property bool active: status == PageStatus.Active
    property bool tabPageActive
    // Capture at portrait aspect ratio to match the 2-column SFOS-style tab cards.
    readonly property real _thumbCaptureWidth: width - Theme.horizontalPageMargin * 2
    readonly property size thumbnailSize: Qt.size(
        _thumbCaptureWidth,
        _thumbCaptureWidth * 1.5 - (Theme.iconSizeSmall + Theme.paddingMedium * 2))
    property Item debug
    property Component tabPageComponent
    property string pendingOpenUrl: ""
    property bool _filePickerOpen: false

    property alias overlay: overlay
    property alias tabs: webView.tabModel
    property alias history: historyModel
    property alias viewLoading: webView.loading
    property alias url: webView.url
    property alias title: webView.title
    property alias webView: webView
    property alias inputRegion: inputRegion

    ConfigurationValue {
        id: adBlockEngine
        key: "/apps/atlantic-browser/settings/adblock_enabled"
        defaultValue: true
        onValueChanged: {
            if (webView.contentItem) {
                webView.contentItem.adBlockEnabled = value
            }
        }
    }

    function load(url, title) {
        overlay.dismiss(true)
        webView.load(url, title)
    }

    function bringToForeground(window) {
        if ((webView.visibility < QuickWindow.Window.Maximized) && window) {
            window.raise()
        }
    }

    function activateNewTabView() {
        // Only open new tab if not blocked MDM, otherwise just bring to foreground
        if (AccessPolicy.browserEnabled) {
            pageStack.pop(browserPage, PageStackAction.Immediate)
            overlay.enterNewTabUrl(PageStackAction.Immediate)
        }
        bringToForeground(webView.chromeWindow)
        // after bringToForeground, webView has focus => activate chrome
        window.activate()
    }

    function openFilePicker() {
        if (_filePickerOpen || !webView.contentItem || !webView.contentItem.fileChooserActive) {
            return
        }
        _filePickerOpen = true
        pageStack.animatorPush(Qt.resolvedUrl("components/FilePickerPage.qml"),
                               { "webView": webView, "browserPage": browserPage })
    }

    // for time being make this fullscreen. TODO: avoid drawing over cutout and corner areas.
    cutoutMode: CutoutMode.FullScreen
    background: null
    onStatusChanged: {
        if (overlay.enteringNewTabUrl || webView.tabModel.count === 0) {
            return
        }

        if (status == PageStatus.Inactive && overlay.visible) {
            overlay.animator.hide()
            overlay.toolBar.certOverlayActive = false
        }
    }

    property int pageOrientation: pageStack.currentPage._windowOrientation
    onPageOrientationChanged: {
        // When on other pages update immediately.
        if (!active) {
            webView.applyContentOrientation(pageOrientation)
        }
    }

    orientationTransitions: orientationFader.orientationTransition

    Keys.onPressed: {
        webView.handleKeyPress(event.key)
    }

    Shared.OrientationFader {
        id: orientationFader

        visible: webView.contentItem
        page: browserPage
        fadeTarget: overlay.animator.allowContentUse ? overlay : overlay.dragArea
        color: webView.contentItem ? (webView.resourceController.videoActive
                                      && webView.contentItem.fullscreen
                                      ? "black"
                                      : (webView.contentItem.backgroundColor || "white"))
                                   : "white"

        onApplyContentOrientation: webView.applyContentOrientation(browserPage.orientation)
    }

    HistoryModel {
        id: historyModel
    }

    Private.VirtualKeyboardObserver {
        id: virtualKeyboardObserver

        active: webView.enabled
        transpose: window._transpose
        orientation: browserPage.orientation

        onWindowChanged: webView.chromeWindow = window

        // Update content height only after virtual keyboard fully opened.
        states: State {
            name: "boundHeightControl"
            when: virtualKeyboardObserver.opened && webView.enabled
            PropertyChanges {
                target: webView.contentItem
                virtualKeyboardHeight: virtualKeyboardObserver.imSize
            }
        }
    }

    QtObject {
        id: maxliveTabs
        property int value: 3
    }

    Browser.DownloadRemorsePopup { id: downloadPopup }

    Shared.WebView {
        id: webView

        enabled: overlay.animator.allowContentUse
        fullscreenHeight: browserPage.height
        portrait: browserPage.isPortrait
        maxLiveTabCount: maxliveTabs.value
        toolbarHeight: overlay.animator.opened ? overlay.toolBar.rowHeight : 0
        rotationHandler: browserPage
        imOpened: virtualKeyboardObserver.opened
        canShowSelectionMarkers: !orientationFader.waitForWebContentOrientationChanged
        historyModel: historyModel

        // Show overlay immediately at top if needed.
        onTabModelChanged: handleModelChanges(true)

        // When a page starts loading, dismiss the overlay so the user can see/interact with content.
        onLoadingChanged: {
            if (loading && !overlay.animator.allowContentUse) {
                overlay.dismiss(true)
            }
        }
        onChromeExposed: {
            if (overlay.animator.atTop && overlay.searchField.focus && !WebUtils.firstUseDone) {
                webView.chromeWindow.raise()
            }
        }

        onForegroundChanged: {
            if (foreground && webView.chromeWindow) {
                webView.chromeWindow.raise()
            }
        }

        onTouched: {
            if (contentFullscreen) {
                fullscreenCloseVisibleTimer.restart()
            }
        }

        onNeedChromeChanged: {
            if (needChrome) {
                overlay.animator.showChrome()
            } else {
                overlay.animator.showFullscreen()
            }
        }

        onWebContentOrientationChanged: orientationFader.waitForWebContentOrientationChanged = false

        function applyContentOrientation(orientation) {
            orientationFader.waitForWebContentOrientationChanged = (contentItem && contentItem.active)

            switch (orientation) {
            case Orientation.None:
            case Orientation.Portrait:
                updateContentOrientation(Qt.PortraitOrientation)
                break
            case Orientation.Landscape:
                updateContentOrientation(Qt.LandscapeOrientation)
                break
            case Orientation.PortraitInverted:
                updateContentOrientation(Qt.InvertedPortraitOrientation)
                break
            case Orientation.LandscapeInverted:
                updateContentOrientation(Qt.InvertedLandscapeOrientation)
                break
            }
        }

        // Both model change and model count change are connected to this.
        function handleModelChanges(openOverlayImmediately) {
            if (webView.completed && (!webView.tabModel || webView.tabModel.count === 0)) {
                overlay.startPage(openOverlayImmediately ? PageStackAction.Immediate
                                                         : PageStackAction.Animated)
            }
        }
    }

    IconButton {
        id: fullscreenClose

        opacity: fullscreenCloseVisibleTimer.running || pressed ? 1.0 : 0.0
        Behavior on opacity { FadeAnimation {} }
        visible: opacity > 0
        x: Theme.paddingLarge
        y: Theme.paddingLarge
        icon.source: "image://theme/icon-m-close"
        onClicked: {
            webView.sendAsyncMessage("embedui:exitFullscreen", {})
        }

        Timer {
            id: fullscreenCloseVisibleTimer

            interval: 2000
            running: webView.contentFullscreen
        }
    }

    // Use Connections so that target updates when model changes.
    Connections {
        target: AccessPolicy.browserEnabled && webView && webView.tabModel || null
        ignoreUnknownSignals: true
        // Animate overlay to top if needed.
        onCountChanged: {
            if (webView.tabModel.count === 0) {
                webView.handleModelChanges(false)
            }
            window.setBrowserCover(webView.tabModel)
        }
    }

    InputRegion {
        id: inputRegion

        window: webView.chromeWindow
        orientation: browserPage.orientation // Qt and Silica orientations match
        // WPE uses a single QWindow for both chrome and web content (unlike Gecko which
        // used a separate QWindow for the web view).  Always expose the full screen so
        // Wayland delivers touch events for the web content area to this window.
        overlayMask: Qt.rect(0, 0, browserPage.width, browserPage.height)
        closeButtonMask: fullscreenClose.visible ? Qt.rect(fullscreenClose.x, fullscreenClose.y,
                                                           fullscreenClose.width, fullscreenClose.height)
                                                 : Qt.rect(0, 0, 0, 0)
    }

    Browser.StartPage {
        id: startPage

        width: browserPage.width
        height: Math.ceil(overlay.y)
        clip: true

        visible: !!webView.tabModel && webView.tabModel.count === 0 && !overlay.toolBar.findInPageActive
        // Stay opaque (no fade) so the empty white web view never flashes through
        // during the transition; only interactive while the overlay is collapsed.
        enabled: visible && overlay.animator.atBottom

        bookmarkModel: overlay.bookmarkModel
        historyModel: historyModel
        overlayOpen: !overlay.animator.atBottom

        // Open the address-bar entry immediately (no slide-up) so it doesn't feel
        // disconnected from the start-page search bar.
        onOpenSearch: overlay.enterNewTabUrl(PageStackAction.Immediate)
        onLoadUrl: overlay.loadPage(url, newTab)
    }

    // Dismiss the open overlay by tapping the area above it. The contentDimmer's
    // own MouseArea can't do this anymore: the dimmer is transparent (and thus
    // not visible / not hit-testable) while the overlay is at top.
    MouseArea {
        id: overlayDismissArea

        property bool inEmptyPrivateMode: webView.privateMode && webView.privateTabModel.count === 0
                                          && webView.persistentTabModel.count > 0

        width: browserPage.width
        height: Math.ceil(overlay.y)
        enabled: overlay.animator.atTop
        onClicked: {
            if (inEmptyPrivateMode) {
                webView.privateMode = false
                //% "Leaving private mode"
                Notices.show(qsTrId("sailfish_browser-la-leaving_private_mode"), Notice.Short, Notice.Top)
            }
            overlay.dismiss(true)
        }
    }

    Browser.DimmerEffect {
        id: contentDimmer

        width: browserPage.width
        height: Math.ceil(overlay.y)

        // Keep the page (or plain black, when no tab is loaded) visible behind the
        // chrome instead of a flat dim: no dimmer when the overlay is fully open
        // (URL entry) or when only the connection-info panel is up.
        dimmerOpacity: (overlay.animator.atBottom
                        || overlay.animator.atTop
                        || overlay.toolBar.certOverlayActive
                        || webView.tabModel.count === 0)
                       ? 0.0
                       : 0.9 - (overlay.y / (webView.fullscreenHeight - overlay.toolBar.rowHeight)) * 0.9

        MouseArea {
            property bool inEmptyPrivateMode: webView.privateMode && webView.privateTabModel.count === 0
                                              && webView.persistentTabModel.count > 0

            anchors.fill: parent
            // Allow dismiss with no tabs too: the start page is behind the overlay.
            enabled: overlay.animator.atTop
            onClicked: {
                if (inEmptyPrivateMode) {
                    webView.privateMode = false
                    //% "Leaving private mode"
                    Notices.show(qsTrId("sailfish_browser-la-leaving_private_mode"), Notice.Short, Notice.Top)
                }
                overlay.dismiss(true)
            }
        }

        Browser.PrivateModeTexture {
            id: privateModeTexture

            anchors.fill: contentDimmer
            visible: webView.privateMode && !overlay.animator.allowContentUse
        }
    }

    Label {
        x: (contentDimmer.width - implicitWidth) / 2
        // Allow only half of the width
        width: parent.width / 2
        truncationMode: TruncationMode.Fade
        opacity: privateModeTexture.visible ? 1.0 : 0.0
        anchors {
            bottom: contentDimmer.bottom
            bottomMargin: (overlay.toolBar.rowHeight - height) / 2
        }

        //: Label for private browsing above address bar
        //% "Private browsing"
        text: qsTrId("sailfish_browser-la-private_mode")
        color: Theme.highlightColor
        font.pixelSize: Theme.fontSizeLarge

        Behavior on opacity { FadeAnimation {} }
    }

    Browser.Overlay {
        id: overlay

        active: browserPage.status == PageStatus.Active && webView.tabModel.loaded
        webView: webView
        historyModel: historyModel
        browserPage: browserPage

        animator.onAtBottomChanged: {
            if (!animator.atBottom) {
                webView.clearSelection()
            }
        }

        onActiveChanged: {
            var isFullScreen = webView.contentItem && webView.contentItem.fullscreen
            if (!isFullScreen && active && !overlay.enteringNewTabUrl) {
                if (webView.hasInitialUrl
                        || webView.tabModel.count !== 0
                        || (WebUtils.homePage !== "about:blank" && WebUtils.homePage.length > 0)) {
                    overlay.animator.showChrome()
                } else {
                    overlay.startPage()
                }
            }

            if (!active) {
                webView.clearSelection()
                if (webView.chromeWindow && webView.foreground) {
                    webView.chromeWindow.raise()
                }
            }
        }
    }

    Browser.PopUpMenu {
        id: popupMenu

        width: parent.width
        height: parent.height
        // Match the start page's wallpaper crop (it scales to overlay.y, not full
        // screen) so the menu's frosted glass lines up with the backdrop behind it.
        wallpaperHeight: Math.ceil(overlay.y)

        active: overlay.toolBar.secondaryToolsActive
        menuItem: Component {
            Browser.PopUpMenuItem {
                iconWidth: Theme.iconSizeMedium + Theme.paddingLarge
            }
        }

        footer: Component {
            Browser.PopUpMenuFooter {
            }
        }

        onClosed: overlay.dismiss(true)
    }

    CoverActionList {
        enabled: browserPage.status === PageStatus.Active
                 || browserPage.tabPageActive
                 || !webView.tabModel
                 || webView.tabModel.count === 0
        iconBackground: true
        window: webView.chromeWindow

        CoverAction {
            iconSource: "image://theme/icon-cover-new"
            onTriggered: activateNewTabView()
        }
    }

    Connections {
        target: WebUtils
        onOpenUrlRequested: {
            // Refuse if blocked by MDM
            if (!AccessPolicy.browserEnabled) {
                bringToForeground(webView.chromeWindow)
                window.activate()
                return
            }

            // Url is empty when user tapped icon when browser was already open.
            // In case first use not done show the overlay immediately.
            if (url == "") {
                bringToForeground(webView.chromeWindow)
                if (!WebUtils.firstUseDone) {
                    overlay.enterNewTabUrl(PageStackAction.Immediate)
                }

                window.activate()
                return
            }

            if (browserPage.status !== PageStatus.Active) {
                pageStack.pop(browserPage, PageStackAction.Immediate)
            }

            webView.grabActivePage()
            if (webView.tabModel.activateTab(url)) {
                webView.releaseActiveTabOwnership()
            } else if (!webView.tabModel.loaded) {
                pendingOpenUrl = url
                overlay.dismiss(true, !Qt.application.active /* immediate */)
            } else {
                webView.clearSelection()
                webView.tabModel.newTab(url, true)
                overlay.dismiss(true, !Qt.application.active /* immediate */)
            }
            bringToForeground(webView.chromeWindow)
            window.activate()
        }
        onActivateNewTabViewRequested: activateNewTabView()
        onShowChrome: {
            pageStack.pop(browserPage, PageStackAction.Immediate)
            overlay.dismiss(true, !Qt.application.active /* immediate */)
            bringToForeground(webView.chromeWindow)
            window.activate()
        }
        onOpenSettingsRequested: {
            pageStack.pop(browserPage, PageStackAction.Immediate)
            pageStack.push(Qt.resolvedUrl("SettingsPage.qml"), {}, PageStackAction.Immediate)
            bringToForeground(webView.chromeWindow)
            window.activate()
        }
        onFirstUseDoneChanged: window.setBrowserCover(webView.tabModel)
    }

    Connections {
        target: webView.tabModel
        onLoadedChanged: {
            if (!webView.tabModel || !webView.tabModel.loaded || pendingOpenUrl === "") {
                return
            }

            var url = pendingOpenUrl
            pendingOpenUrl = ""

            if (webView.tabModel.activateTab(url)) {
                webView.releaseActiveTabOwnership()
            } else {
                webView.clearSelection()
                webView.tabModel.newTab(url, true)
            }
        }
    }

    Connections {
        target: webView.contentItem
        ignoreUnknownSignals: true
        onFileChooserActiveChanged: {
            if (!webView.contentItem) {
                _filePickerOpen = false
                return
            }

            if (webView.contentItem.fileChooserActive) {
                openFilePicker()
            } else {
                _filePickerOpen = false
                if (pageStack.currentPage && pageStack.currentPage.objectName === "atlanticFilePickerPage") {
                    pageStack.pop(pageStack.currentPage, PageStackAction.Immediate)
                }
            }
        }
    }

    // Tab crash banner — shown when the WebProcess for the active tab crashes.
    // Crash banner, HTML <select> dropdown, and image long-press panel —
    // extracted into their own components (kept this page from sprawling).
    Browser.CrashBanner { webView: webView }

    Browser.SelectMenuOverlay { webView: webView }

    Browser.ImageActionPanel { webView: webView }

    Component.onCompleted: {
        window.setBrowserCover(webView.tabModel)
        if (Qt.application.arguments.indexOf("-debugMode") > 0) {
            var component = Qt.createComponent(Qt.resolvedUrl("components/DebugOverlay.qml"))
            if (component.status === Component.Ready) {
                debug = component.createObject(browserPage)
            } else {
                console.warn("Failed to create DebugOverlay " + component.errorString())
            }
        }
    }
}
