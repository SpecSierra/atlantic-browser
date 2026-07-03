/****************************************************************************
**
** Copyright (c) 2013 - 2021 Jolla Ltd.
** Copyright (c) 2019 - 2020 Open Mobile Platform LLC.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import Sailfish.Silica 1.0
import "pages"
import "shared"

BrowserWindow {
    id: window

    // Websites' prefers-color-scheme follows the ambience (or the explicit
    // light/dark choice in Settings — SettingManager combines both).
    readonly property bool _ambienceDark: Theme.colorScheme === Theme.LightOnDark
    on_AmbienceDarkChanged: Settings.setAmbienceDark(_ambienceDark)
    Component.onCompleted: Settings.setAmbienceDark(_ambienceDark)

    function setBrowserCover(model) {
        if (!model || model.count === 0 || !WebUtils.firstUseDone) {
            cover = Qt.resolvedUrl("cover/NoTabsCover.qml")
        } else {
            if (cover != null && window.webView) {
                // clearSurface is Gecko-specific, not available in WPE
            }
            cover = null
        }
    }

    //% "Web browsing"
    activityDisabledByMdm: qsTrId("sailfish_browser-la-web_browsing")
    initialPage: Component {
        BrowserPage {
            id: browserPage

            Component.onCompleted: {
                window.webView = webView
                window.rootPage = browserPage
            }

            Component.onDestruction: {
                window.webView = null
            }
        }
    }
}
