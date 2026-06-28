/****************************************************************************
**
** Copyright (c) 2026 Atlantic Browser
** SPDX-License-Identifier: MPL-2.0
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0

Dialog {
    id: dialog

    // Set by the caller (DownloadManager.saveAsRequested).
    property int downloadId: -1
    property string suggestedFileName
    property string folder

    // Guards onRejected so we only cancel the download if the user backed out
    // without accepting.
    property bool _confirmed: false

    canAccept: nameField.text.trim().length > 0

    onAccepted: {
        _confirmed = true
        var dir = folder
        while (dir.length > 1 && dir[dir.length - 1] === "/")
            dir = dir.substring(0, dir.length - 1)
        DownloadManager.confirmDownload(downloadId, dir + "/" + nameField.text.trim())
    }

    onRejected: {
        if (!_confirmed)
            DownloadManager.cancelPendingDownload(downloadId)
    }

    Column {
        width: parent.width

        DialogHeader {
            //% "Save"
            acceptText: qsTrId("sailfish_browser-he-save_download")
            //% "Save file"
            title: qsTrId("sailfish_browser-he-save_file")
        }

        TextField {
            id: nameField
            width: parent.width
            text: dialog.suggestedFileName
            //% "File name"
            label: qsTrId("sailfish_browser-la-file_name")
            //% "File name"
            placeholderText: qsTrId("sailfish_browser-la-file_name")
            EnterKey.iconSource: "image://theme/icon-m-enter-close"
            EnterKey.onClicked: focus = false
        }

        ValueButton {
            width: parent.width
            //% "Location"
            label: qsTrId("sailfish_browser-la-location")
            value: dialog.folder
            onClicked: {
                var picker = pageStack.animatorPush(folderPickerComponent, { path: dialog.folder })
                picker.pageCompleted.connect(function(page) {
                    page.accepted.connect(function() {
                        if (page.selectedPath)
                            dialog.folder = page.selectedPath
                    })
                })
            }
        }
    }

    Component {
        id: folderPickerComponent
        FolderPickerDialog {
            //% "Save in"
            title: qsTrId("sailfish_browser-he-save_in")
        }
    }
}
