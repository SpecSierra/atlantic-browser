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
import Nemo.Configuration 1.0
import "pages"
import "shared"

BrowserWindow {
    id: window

    // Websites' prefers-color-scheme: explicit light/dark from Settings, or
    // follow the ambience. Combined here in QML because SettingManager's
    // MDConfItem is a no-op stub (its dconf read/watch never fires), so the
    // setting must be read via ConfigurationValue and the *effective* dark
    // flag pushed through Settings.setAmbienceDark.
    readonly property bool _ambienceDark: Theme.colorScheme === Theme.LightOnDark
    // 0 = light, 1 = dark, 2 = follow ambience (matches SettingsPage.qml)
    readonly property bool _websiteDark: colorSchemeConf.value === 1
                                         || (colorSchemeConf.value === 2 && _ambienceDark)
    on_WebsiteDarkChanged: Settings.setAmbienceDark(_websiteDark)
    Component.onCompleted: Settings.setAmbienceDark(_websiteDark)

    ConfigurationValue {
        id: colorSchemeConf
        key: "/apps/atlantic-browser/settings/color_scheme"
        defaultValue: 2
    }

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
