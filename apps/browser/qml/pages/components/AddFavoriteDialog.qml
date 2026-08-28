/****************************************************************************
**
** Copyright (c) 2026 SpecSierra
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.6
import Sailfish.Silica 1.0
import "." as Browser
import "UrlUtils.js" as UrlUtils

// Add a link to the start page grid by typing it, rather than having to visit
// the page and bookmark it. The "+" tile used to open the address bar, which
// navigates somewhere instead of filing it.
Dialog {
    id: dialog

    property var bookmarkModel
    // The folder the grid is showing. add() always appends at the root, so a
    // new favourite has to be filed afterwards or it would not appear in the
    // grid that asked for it.
    property string folderId: ""

    property string enteredUrl
    property string enteredTitle

    canAccept: enteredUrl.trim().length > 0

    onAccepted: {
        if (!bookmarkModel)
            return

        // Same normalisation the address bar uses, so a bare host works here
        // too; without it "example.com" is stored unloadable.
        var url = UrlUtils.normalize(enteredUrl.trim(), "")
        var title = enteredTitle.trim() || url
        // add() only started returning the new id alongside folders; guard it
        // so this still works against a build where it returns nothing (the
        // favourite lands at the root, which is where the grid looks anyway
        // until a folder is chosen).
        var id = bookmarkModel.add(url, title, "", true)
        if (folderId.length > 0 && id && id.length > 0)
            bookmarkModel.setParentId(id, folderId)
    }

    Column {
        width: parent.width

        DialogHeader {
            //% "Add"
            acceptText: qsTrId("atlantic-he-add_favorite_accept")
        }

        TextField {
            width: parent.width
            //% "Web page address"
            label: qsTrId("settings_browser-la-web_page_address")
            placeholderText: label
            inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase | Qt.ImhUrlCharactersOnly
            EnterKey.iconSource: "image://theme/icon-m-enter-next"
            EnterKey.onClicked: titleField.forceActiveFocus()
            onTextChanged: enteredUrl = text
            Component.onCompleted: forceActiveFocus()
        }

        TextField {
            id: titleField

            width: parent.width
            //% "Name (optional)"
            label: qsTrId("atlantic-la-favorite_name")
            placeholderText: label
            inputMethodHints: Qt.ImhNoPredictiveText
            EnterKey.iconSource: "image://theme/icon-m-enter-accept"
            EnterKey.onClicked: dialog.accept()
            onTextChanged: enteredTitle = text
        }
    }
}
