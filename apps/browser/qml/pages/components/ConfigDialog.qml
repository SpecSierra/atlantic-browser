/****************************************************************************
**
** Copyright (c) 2014
** Contact: Siteshwar Vashisht <siteshwar AT gmail.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */


import QtQuick 2.2
import Sailfish.Silica 1.0

// WPE: about:config removed (Gecko-specific). This dialog is not available with WPE WebKit.
Dialog {
    id: configDialog

    property var changedConfigs: ({})
    // WPE stubs
    QtObject {
        id: _webEngineSettings
        readonly property int BoolPref: 0
        readonly property int IntPref: 1
        function setPreference(key, value, type) {}
    }

    Column {
        width: parent.width
        DialogHeader { title: "about:config"; dialog: configDialog }
        Label {
            x: Theme.horizontalPageMargin
            width: parent.width - 2*Theme.horizontalPageMargin
            text: "about:config is not available with WPE WebKit"
            color: Theme.secondaryHighlightColor
            wrapMode: Text.Wrap
        }
    }
}
