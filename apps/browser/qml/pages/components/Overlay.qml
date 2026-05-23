/*
 * Copyright (c) 2014 - 2021 Jolla Ltd.
 * Copyright (c) 2019 - 2021 Open Mobile Platform LLC.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

import QtQuick 2.2
import QtQuick.Window 2.2 as QuickWindow
import Sailfish.Silica 1.0
import Sailfish.Silica.private 1.0 as Private
import Sailfish.Browser 1.0
import Sailfish.Policy 1.0
import Sailfish.WebView.Controls 1.0
import Sailfish.WebView.Popups 1.0
import com.jolla.settings.system 1.0
import "." as Browser
import "../../shared" as Shared

Shared.Background {
    id: overlay

    property bool active
    property QtObject webView
    property Item browserPage
    property var historyModel
    property alias bookmarkModel: bookmarkModel
    property alias toolBar: toolBar
    property alias progressBar: progressBar
    property alias animator: overlayAnimator
    property alias dragArea: dragArea
    property alias searchField: searchField
    readonly property alias enteringNewTabUrl: searchField.enteringNewTabUrl
    property var favoriteGrid: historyList.headerItem
    property string enteredUrl

    property real _overlayGap: browserPage.isPortrait ? toolBar.rowHeight : 0
    property bool _showFindInPage
    property bool _showUrlEntry
    readonly property bool _topGap: _showUrlEntry || _showFindInPage
    property int _biggestCorner: Math.max(Screen.topLeftCorner.radius,
                                          Screen.topRightCorner.radius,
                                          Screen.bottomLeftCorner.radius,
                                          Screen.bottomRightCorner.radius)
    property int horizontalMargin: Math.max(Theme.paddingLarge,
                                            _biggestCorner * 0.7,
                                            browserPage.isLandscape
                                            ? (Screen.topCutout.height + Theme.paddingSmall)
                                            : 0)

    // WPE: detect whether input is a URL or a search query
    function _isUrl(text) {
        text = text.trim()
        if (text.indexOf("://") !== -1) return true
        if (text.indexOf("about:") === 0) return true
        if (/^[a-zA-Z0-9\-]+\.[a-zA-Z]{2,}/.test(text)) return true
        if (/^localhost(:[0-9]+)?/.test(text)) return true
        if (/^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+/.test(text)) return true
        return false
    }

    function loadPage(url, newTab) {
        console.log("[QML-LOAD] loadPage url=" + url + " newTab=" + newTab)
        if (url == "about:config") {
            pageStack.animatorPush(Qt.resolvedUrl("ConfigWarning.qml"), {"browserPage": browserPage})
        } else if (url == "about:settings") {
            pageStack.animatorPush(Qt.resolvedUrl("../SettingsPage.qml"))
        } else {
            var pageUrl = url.trim()
            // WPE: convert search keywords to search URL
            if (pageUrl && !_isUrl(pageUrl)) {
                pageUrl = "https://www.google.com/search?q=" + encodeURIComponent(pageUrl)
            } else if (pageUrl && pageUrl.indexOf("://") === -1 && pageUrl.indexOf("about:") !== 0) {
                pageUrl = "https://" + pageUrl
            }

            console.log("[QML-LOAD] resolved pageUrl=" + pageUrl)
            webView.load(pageUrl)
        }

        overlayAnimator.showChrome()
    }

    function enterNewTabUrl(action) {
        searchField.enteringNewTabUrl = true
        _showUrlEntry = true
        _overlayGap = Qt.binding(function () { return overlayAnimator.fullscreenGap })
        searchField.resetUrl("")
        overlayAnimator.showOverlay(action === PageStackAction.Immediate)
    }

    function startPage(action) {
        searchField.enteringNewTabUrl = true
        _showUrlEntry = true
        _overlayGap = Qt.binding(function () { return overlayAnimator.fullscreenGap })
        searchField.resetUrl("")
        overlayAnimator.showStartPage(action !== PageStackAction.Animated)
    }

    function dismiss(canShowChrome, immediate) {
        toolBar.resetFind()
        if (webView.contentItem && webView.contentItem.fullscreen) {
            // Web content is in fullscreen mode thus we don't show chrome
            overlay.animator.showFullscreen()
        } else if (canShowChrome) {
            overlay.animator.showChrome(immediate)
        } else {
            overlay.animator.hide()
        }
        searchField.enteringNewTabUrl = false
    }

    y: webView.fullscreenHeight - toolBar.rowHeight
    width: parent.width
    height: historyContainer.height + virtualKeyboardObserver.panelSize
    // `visible` is controlled by Browser.OverlayAnimator
    enabled: visible

    Private.VirtualKeyboardObserver {
        id: virtualKeyboardObserver

        active: overlay.active && !overlayAnimator.atBottom
        orientation: browserPage.orientation
    }

    // This is an invisible object responsible to hide/show Overlay in an animated way
    Shared.OverlayAnimator {
        id: overlayAnimator

        overlay: overlay
        portrait: browserPage.isPortrait
        webView: overlay.webView
        fullscreenGap: isPortrait ? (overlay.toolBar.rowHeight + Screen.topCutout.height) : 0
        infoHeight: Math.max(0,
                             webView.fullscreenHeight
                             - overlay.toolBar.certOverlayPreferedHeight - overlay.toolBar.rowHeight)

        onAtBottomChanged: {
            if (atBottom) {
                searchField.enteringNewTabUrl = false

                if (enteredUrl) {
                    webView.tabModel.newTab(enteredUrl, true)
                    enteredUrl = ""
                } else if (!toolBar.findInPageActive) {
                    searchField.resetUrl(webView.url)
                }

                favoriteGrid.positionViewAtBeginning()
                historyList.positionViewAtBeginning()

                if (!WebUtils.firstUseDone) {
                    WebUtils.firstUseDone = true
                }

                _showFindInPage = false
                _showUrlEntry = false
                toolBar.certOverlayActive = false
            }
            dragArea.moved = false
        }

        onAtTopChanged: {
            if (atTop) {
                if (_showFindInPage || _showUrlEntry) {
                    toolBar.certOverlayActive = false
                }
            } else if (!toolBar.certOverlayActive) {
                dragArea.moved = true
            }
        }
    }

    Connections {
        target: webView

        onLoadingChanged: {
            if (webView.loading) {
                toolBar.resetFind()
            }
        }

        onUrlChanged: {
            if (!toolBar.findInPageActive && !searchField.enteringNewTabUrl && !searchField.edited) {
                searchField.resetUrl(webView.url)
            }
        }
    }

    BookmarkModel {
        id: bookmarkModel

        activeUrl: toolBar.url
    }

    MouseArea {
        id: dragArea

        property bool moved
        property int dragThreshold: state === "fullscreenOverlay"
                                    ? toolBar.rowHeight * 1.5
                                    : state === "certOverlay"
                                      ? (overlayAnimator.infoHeight + toolBar.rowHeight * 0.5)
                                      : (webView.fullscreenHeight - toolBar.rowHeight * 2)

        width: parent.width
        height: historyContainer.height
        enabled: !overlayAnimator.atBottom && webView.tabModel.count > 0 && !favoriteGrid.contextMenuActive

        drag.target: overlay
        drag.filterChildren: true
        drag.axis: Drag.YAxis
        // Favorite grid first row offset is negative. So, increase minimumY drag by that.
        drag.minimumY: _overlayGap
        drag.maximumY: webView.fullscreenHeight - toolBar.rowHeight

        drag.onActiveChanged: {
            if (!drag.active) {
                if (overlay.y < dragThreshold || overlayAnimator.direction === "upwards") {
                    if (state === "certOverlay") {
                        overlayAnimator.showInfoOverlay(false)
                    } else {
                        overlayAnimator.showOverlay(false)
                    }
                } else {
                    dismiss(true)
                }
            } else {
                // Store previous end state
                if (!overlayAnimator.dragging) {
                    state = overlayAnimator.state
                }

                overlayAnimator.drag()
            }
        }

        Shared.ProgressBar {
            id: progressBar

            width: parent.width
            height: toolBar.rowHeight
            visible: !searchField.enteringNewTabUrl
            opacity: webView.loading ? 1.0 : 0.0
            progress: webView.loadProgress / 100.0
        }

        Item {
            id: historyContainer

            readonly property bool showFavorites: !overlayAnimator.atBottom
                                                  && !toolBar.findInPageActive
                                                  && _showUrlEntry
            readonly property bool showHistoryList: showFavorites
                                                    && searchField.edited
                                                    && searchField.text !== webView.url
                                                    && searchField.text
            readonly property bool showHistoryButton: !toolBar.findInPageActive
                                                      && (!searchField.edited && searchField.text === webView.url
                                                          || !searchField.text)

            width: parent.width
            height: toolBar.rowHeight + historyList.height
            // Clip only when content has been moved and we're at top or animating downwards.
            clip: (overlayAnimator.atTop
                   || overlayAnimator.direction === "downwards"
                   || overlayAnimator.direction === "upwards"
                   || favoriteGrid.opacity != 0.0
                   || historyList.opacity != 0.0)
                  && searchField.y < 0

            PrivateModeTexture {
                opacity: toolBar.visible && webView.privateMode ? toolBar.opacity : 0.0
            }

            Loader {
                id: textSelectionToolbar

                width: parent.width
                height: isPortrait ? toolBar.scaledPortraitHeight : toolBar.scaledLandscapeHeight
                active: webView.contentItem && webView.contentItem.textSelectionActive && !toolBar.findInPageActive

                opacity: active ? 1.0 : 0.0
                Behavior on opacity {
                    FadeAnimator {}
                }

                onActiveChanged: {
                    if (active) {
                        overlayAnimator.showChrome(false)
                        if (webView.contentItem) {
                            webView.contentItem.forceChrome(true)
                        }
                    } else if (!toolBar.findInPageActive) {
                        if (webView.contentItem) {
                            webView.contentItem.forceChrome(false)
                        }
                    }
                }

                sourceComponent: Component {
                    Row {
                        id: selToolbar
                        property var ci: webView ? webView.contentItem : null
                        width: textSelectionToolbar.width
                        height: Theme.itemSizeSmall
                        spacing: 0

                        IconButton {
                            width: selToolbar.width / 3
                            height: parent.height
                            icon.source: "image://theme/icon-m-clipboard"
                            onClicked: {
                                if (selToolbar.ci) {
                                    selToolbar.ci.copyToClipboard()
                                    selToolbar.ci.clearSelection()
                                }
                            }
                            Label { anchors.horizontalCenter: parent.horizontalCenter; y: parent.icon.y + parent.icon.height - Theme.paddingSmall; text: "Copy"; font.pixelSize: Theme.fontSizeTiny }
                        }
                        IconButton {
                            width: selToolbar.width / 3
                            height: parent.height
                            icon.source: "image://theme/icon-m-select-all"
                            onClicked: { if (selToolbar.ci) selToolbar.ci.selectAll() }
                            Label { anchors.horizontalCenter: parent.horizontalCenter; y: parent.icon.y + parent.icon.height - Theme.paddingSmall; text: "Select All"; font.pixelSize: Theme.fontSizeTiny }
                        }
                        IconButton {
                            width: selToolbar.width / 3
                            height: parent.height
                            icon.source: "image://theme/icon-m-input-remove"
                            onClicked: { if (selToolbar.ci) selToolbar.ci.clearSelection() }
                            Label { anchors.horizontalCenter: parent.horizontalCenter; y: parent.icon.y + parent.icon.height - Theme.paddingSmall; text: "Clear"; font.pixelSize: Theme.fontSizeTiny }
                        }
                    }
                }
            }

            TextField {
                id: searchField

                readonly property bool requestingFocus: AccessPolicy.browserEnabled
                                                        && overlayAnimator.atTop
                                                        && browserPage.active
                                                        && !dragArea.moved
                                                        && (_showFindInPage || _showUrlEntry)

                // Release focus when ever history list or favorite grid is moved and overlay itself starts moving
                // from the top. After moving the overlay or the content, search field can be focused by tapping.
                readonly property bool focusOut: dragArea.moved

                readonly property bool browserEnabled: AccessPolicy.browserEnabled
                onBrowserEnabledChanged: if (!browserEnabled) focus = false

                property bool edited
                property bool enteringNewTabUrl
                property bool _resetting

                property string lastFindText

                function resetUrl(url) {
                    _resetting = true
                    // Reset first text and then mark as unedited.
                    text = url === "about:blank" ? "" : url || ""
                    _resetting = false
                    edited = false
                }

                function hasMoved() {
                    return y < -height && historyList.contentY !== favoriteGrid.contentY
                }

                // Follow grid / list position.
                y: -((historyContainer.showHistoryList
                      ? (favoriteGrid.count > 0
                         ? historyList.contentY + favoriteGrid.cellHeight + favoriteGrid.menuHeight
                         : historyList.contentY)
                      : favoriteGrid.contentY)
                     + favoriteGrid.headerItem.height + favoriteGrid.menuHeight)

                // On top of HistoryList and FavoriteGrid
                z: 1
                height: toolBar.rowHeight
                textLeftMargin: overlay.horizontalMargin
                textRightMargin: overlay.horizontalMargin
                focusOutBehavior: FocusBehavior.ClearPageFocus
                font {
                    pixelSize: Theme.fontSizeLarge
                    family: Theme.fontFamilyHeading
                }

                textTopMargin: height/2 - _editor.implicitHeight/2
                labelVisible: false
                inputMethodHints: Qt.ImhUrlCharactersOnly
                background: Rectangle {
                    anchors.fill: parent
                    color: Theme.primaryColor
                    opacity: 0.1
                }

                placeholderText: toolBar.findInPageActive
                                 ? //: Placeholder text for finding text from the web page
                                   //% "Find from page"
                                   qsTrId("sailfish_browser-ph-type_find_from_page")
                                 : //: Placeholder text for url typing and searching
                                   //% "Type URL or search"
                                   qsTrId("sailfish_browser-ph-type_url_or_search")

                EnterKey.onClicked: {
                    if (!text) {
                        return
                    }

                    if (toolBar.findInPageActive) {
                        lastFindText = text
                        if (webView.contentItem) webView.contentItem.findText(text, false)
                        overlayAnimator.showChrome()
                    } else {
                        overlay.loadPage(text)
                    }
                }

                opacity: toolBar.crossfadeRatio * -1.0
                visible: opacity > 0.0 && y >= -searchField.height

                onYChanged: {
                    if (hasMoved()) {
                        // the binding evaluation order might result in unexpected temporary positions
                        // redo the move check after all of them are handled
                        moveCheckTimer.start()
                    }
                }

                onRequestingFocusChanged: {
                    if (requestingFocus) {
                        forceActiveFocus()
                    }
                }

                onFocusOutChanged: {
                    if (focusOut) {
                        overlay.focus = true
                    }
                }

                onFocusChanged: {
                    if (focus) {
                        cursorPosition = text.length
                        if (text.length > 0) {
                            searchField.selectAll()
                        }
                        dragArea.moved = false
                    }
                }

                onTextChanged: {
                    if (!_resetting && !edited && text !== webView.url) {
                        edited = true
                    }
                }

                Timer {
                    id: moveCheckTimer

                    interval: 0
                    onTriggered: {
                        if (searchField.hasMoved()) {
                            dragArea.moved = true
                        }
                    }
                }
            }

            OverlayListItem {
                id: historyButton

                height: historyContainer.showHistoryButton ? toolBar.rowHeight : 0
                iconWidth: toolBar.iconWidth
                leftMargin: overlay.horizontalMargin
                anchors.top: searchField.bottom
                // On top of HistoryList and FavoriteGrid
                z: 1

                text: qsTrId("sailfish_browser-la-history")
                iconSource: "image://theme/icon-m-history"
                visible: opacity > 0
                opacity: (historyContainer.showHistoryButton && toolBar.opacity < 0.9) ? 1.0 : 0.0

                onClicked: {
                    var historyPage = pageStack.push("../HistoryPage.qml", { model: historyModel })
                    historyPage.loadPage.connect(loadPage)
                    historyPage.saveBookmark.connect(function(url, title, favicon) {
                        bookmarkModel.add(url, title || url, favicon, true)
                    })
                }
            }

            // Below the HistoryList and FavoriteGrid to let dragging to work
            // when finding from the page active or favorite grid enabled (at top).
            // On large screens favorite grid is not covering fullscreen. Thus,
            // this needs to be enabled so that overlay can be dragged from edges.
            MouseArea {
                anchors {
                    fill: historyList
                    topMargin: _topGap ? searchField.height : 0
                    bottomMargin: _topGap ? 0 : searchField.height
                }
                enabled: toolBar.findInPageActive || favoriteGrid.enabled

                ViewPlaceholder {
                    // The parent is a sibling of the historyList. Hence,
                    // flickable binding.
                    flickable: historyList
                    x: (historyList.width - width) / 2
                    y: favoriteGrid.originY + (historyList.height - height) / 2

                    enabled: toolBar.findInPageActive && searchField.text

                    //: View placeholder text for find-in-page search.
                    //% "Press enter to search"
                    text: qsTrId("sailfish_browser-la-press_enter_to_search")
                }
            }

            Browser.ToolBar {
                id: toolBar

                property real crossfadeRatio: (_showFindInPage || _showUrlEntry)
                                              ? (overlay.y - webView.fullscreenHeight/2)
                                                / (webView.fullscreenHeight/2 - toolBar.height)
                                              : 1.0

                url: webView.contentItem && webView.contentItem.url || ""
                findText: searchField.text
                bookmarked: bookmarkModel.activeUrlBookmarked

                opacity: textSelectionToolbar.active ? 0.0 : crossfadeRatio
                Behavior on opacity {
                    enabled: overlayAnimator.atBottom
                    FadeAnimation {}
                }

                visible: opacity > 0.0
                secondaryToolsActive: overlayAnimator.secondaryTools
                certOverlayHeight: !toolBar.certOverlayActive
                                   ? 0
                                   : Math.max((webView.fullscreenHeight - overlay.y - overlay.toolBar.rowHeight)
                                              - overlay.toolBar.secondaryToolsHeight, 0)

                certOverlayAnimPos: Math.min(Math.max((webView.fullscreenHeight - overlay.y - overlay.toolBar.rowHeight)
                                                      / (webView.fullscreenHeight - overlayAnimator.infoHeight
                                                         - overlay.toolBar.rowHeight), 0.0), 1.0)

                onShowOverlay: {
                    _showUrlEntry = true
                    _overlayGap = Qt.binding(function() { return overlayAnimator.fullscreenGap })
                    searchField.resetUrl(webView.url)
                    overlayAnimator.showOverlay()
                }
                onShowTabs: {
                    // Push the currently active tab index.
                    // Changing of active tab cannot cause blinking.
                    webView.grabActivePage()
                    pageStack.animatorPush(tabView)
                }
                onShowSecondaryTools: overlayAnimator.showSecondaryTools()
                onShowInfoOverlay: {
                    toolBar.certOverlayActive = true
                    _overlayGap = Qt.binding(function() { return overlayAnimator.infoHeight })
                    overlayAnimator.showInfoOverlay(false)
                }
                onShowChrome: overlayAnimator.showChrome()

                onLoadPage: overlay.loadPage(url)
                onEnterNewTabUrl: overlay.enterNewTabUrl()
                onFindInPage: {
                    _showFindInPage = true
                    if (searchField.lastFindText.length > 0) {
                        searchField.resetUrl(searchField.lastFindText)
                    } else {
                        searchField.resetUrl("")
                    }
                    _overlayGap = Qt.binding(function () { return overlayAnimator.fullscreenGap })
                    overlayAnimator.showOverlay()
                }
                onShareActivePage: webShareAction.shareLink(webView.url, webView.title)
                onBookmarkActivePage: favoriteGrid.fetchAndSaveBookmark()
                onRemoveActivePageFromBookmarks: bookmarkModel.remove(webView.url)

                onShowCertDetail: {
                    var sec = webView.contentItem ? webView.contentItem.security : null
                    if (sec && !sec.certIsNull) {
                        pageStack.push(Qt.resolvedUrl("CertificateDetailsPage.qml"), { "security": sec })
                    }
                }
            }

            Browser.HistoryList {
                id: historyList

                property int panelSize: favoriteGrid.contextMenu && favoriteGrid.contextMenu.active
                                        ? 0 : virtualKeyboardObserver.panelSize

                width: parent.width
                height: browserPage.height - _overlayGap - panelSize
                footerHeight: historyContainer.showHistoryList ? Theme.itemSizeLarge : 0
                horizontalMargin: overlay.horizontalMargin

                header: Browser.FavoriteGrid {
                    id: favoriteGrid

                    // horizontalMargin omitted, favoriteGrid has it internally good enough
                    height: historyContainer.showHistoryList ? (count > 0
                                                                ? cellHeight + headerItem.height + menuHeight
                                                                : headerItem.height)
                                                             : historyList.height
                    opacity: visible && toolBar.opacity < 0.9 ? 1.0 : 0.0
                    enabled: overlayAnimator.atTop
                    visible: historyContainer.showFavorites
                    _quickScrollRightMargin: -(browserPage.width - width) / 2
                    footerHeight: historyContainer.showHistoryList ? 0 : Theme.paddingLarge

                    header: Item {
                        width: parent.width
                        height: searchField.height + historyButton.height
                    }

                    model: BookmarkFilterModel {
                        sourceModel: bookmarkModel
                        maxDisplayedItems: search ? favoriteGrid.columns : bookmarkModel.count
                        search: historyContainer.showHistoryList ? searchField.text : ""
                    }

                    onMovingChanged: if (moving) favoriteGrid.focus = true
                    onLoad: overlay.loadPage(url)
                    onShare: webShareAction.shareLink(url, title)
                    onNewTab: {
                        searchField.resetUrl(url)
                        // Not the best property name but functionality of opening a favorite
                        // to a new tab is exactly the same as opening new tab by typing a url.
                        searchField.enteringNewTabUrl = true
                        _showUrlEntry = true
                        overlay.loadPage(url)
                    }

                    Behavior on opacity { FadeAnimator {} }
                }

                search: searchField.text
                opacity: visible && toolBar.opacity < 0.9 ? 1.0 : 0.0
                enabled: overlayAnimator.atTop
                visible: !overlayAnimator.atBottom && _showUrlEntry
                onMovingChanged: if (moving) historyList.focus = true
                onSearchChanged: if (search !== webView.url) historyModel.search(search)
                model: historyContainer.showHistoryList ? historyModel : 0
                contentY: favoriteGrid.y
                showDeleteButton: true
                onLoad: {
                    historyList.focus = true
                    overlay.loadPage(url, newTab)
                }
                // necessary for correct display of context menu of FavoriteGrid
                onContentHeightChanged: if (menuClosed) contentY = favoriteGrid.y
                onSaveBookmark: bookmarkModel.add(url, title || url, favicon, true)

                viewPlaceholder.enabled: historyList.model && !historyList.model.count

                Behavior on opacity { FadeAnimator {} }
            }
        }
    }

    Component {
        id: tabView

        Page {
            id: tabPage

            onStatusChanged: browserPage.tabPageActive = (status == PageStatus.Active)
            cutoutMode: CutoutMode.FullScreen

            Browser.TabView {
                id: tabViewItem

                tabModel: webView.tabModel
                portrait: tabPage.isPortrait
                privateMode: webView.privateMode

                scaledPortraitHeight: toolBar.scaledPortraitHeight
                scaledLandscapeHeight: toolBar.scaledLandscapeHeight
                horizontalMargin: Math.max(Theme.horizontalPageMargin,
                                           tabPage.isLandscape ? (Screen.topCutout.height + Theme.paddingSmall) : 0)

                onHide: pageStack.pop()

                onPrivateModeChanged: {
                    webView.privateMode = privateMode
                    if (webView.tabModel.count === 0) {
                        overlay.enterNewTabUrl(PageStackAction.Immediate)
                    } else if (!overlayAnimator.atBottom) {
                        // Hide overlay while switching to non-empty tabmodel
                        // Dismiss overlay so that chrome gets hidden.
                        // Chrome is animated back when BrowserPage's activates.
                        dismiss(false)
                    }
                }

                onEnterNewTabUrl: {
                    if (webView.tabModel.count === 0) {
                        overlay.startPage()
                    } else {
                        overlay.enterNewTabUrl(PageStackAction.Immediate)
                    }
                    pageStack.pop()
                }
                onActivateTab: {
                    webView.tabModel.activateTab(index)
                    pageStack.pop()
                }
                onCloseTab: {
                    webView.tabModel.remove(index)
                    if (webView.tabModel.count === 0) {
                        overlay.startPage()
                    }
                }

                onCloseAllPending: overlay.enterNewTabUrl(PageStackAction.Immediate)
                onCloseAllCanceled: overlay.dismiss(true /* show chrome */, true /* immediate */)
                onCloseAll: {
                    webView.tabModel.clear()
                    overlay.startPage()
                }
            }
        }
    }

    WebShareAction {
        id: webShareAction
    }
}
