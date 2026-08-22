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
import "cover"

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

    // Covers are instantiated by Silica from ApplicationWindow's own context, so
    // a cover loaded by url cannot see anything in this file. Loaders keep them
    // in this context, where the web view can be bound to them.
    //
    // They are also handed over as ready-made Items rather than as a url or a
    // Component: CoverLoader.js incubates those, and its incubation callback
    // dies with "TypeError: Cannot read property 'Ready' of undefined" (the
    // Component type does not resolve inside that callback on this Qt), so the
    // cover object is never installed and lipstick falls back to its default
    // icon tile. Passing an existing Item takes CoverLoader's non-incubating
    // branch and actually shows the cover.
    Loader {
        id: noTabsCoverLoader

        active: false
        sourceComponent: NoTabsCover {
            visible: false
        }
    }

    Loader {
        id: browserCoverLoader

        active: false
        sourceComponent: BrowserCover {
            visible: false
            webView: window.webView
        }
    }

    function setBrowserCover(model) {
        if (!model || model.count === 0 || !WebUtils.firstUseDone) {
            noTabsCoverLoader.active = true
            cover = noTabsCoverLoader.item
            browserCoverLoader.active = false
        } else {
            browserCoverLoader.active = true
            cover = browserCoverLoader.item
            noTabsCoverLoader.active = false
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
