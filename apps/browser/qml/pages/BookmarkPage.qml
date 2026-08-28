/****************************************************************************
**
** Copyright (c) 2020 Open Mobile Platform LLC
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Browser 1.0
import Nemo.Configuration 1.0
import Sailfish.Silica.Background 1.0 as SilicaBackground
import "components"

Page {
    id: page

    SilicaBackground.Background {
        anchors.fill: parent
        z: -1
    }

    property string searchText
    property BookmarkModel bookmarkModel

    // Which folder this page is showing. "" is the root; opening a folder
    // pushes another copy of this page rather than filtering in place, so the
    // pull-back gesture walks back up the tree for free.
    property string folderId: ""
    property string folderTitle

    // Browsing is per folder; searching is not. A search that only looked
    // inside the folder you happen to be standing in would hide the very
    // bookmark you filed somewhere and cannot find.
    readonly property bool searching: searchText.length > 0

    BookmarkFilterModel {
        id: bookmarkFilterModel

        sourceModel: bookmarkModel
        search: searchText
    }

    BookmarkFolderModel {
        id: bookmarkFolderModel

        sourceModel: bookmarkModel
        folderId: page.folderId
    }

    ConfigurationValue {
        id: startPageFolder

        key: "/apps/atlantic-browser/settings/start_page_folder"
        defaultValue: ""
    }

    SilicaListView {
        id: listView

        anchors.fill: parent
        model: page.searching ? bookmarkFilterModel : bookmarkFolderModel
        currentIndex: -1

        PullDownMenu {
            MenuItem {
                //% "Show this folder on the start page"
                text: qsTrId("atlantic-me-folder_on_start_page")
                visible: !page.searching && startPageFolder.value !== page.folderId
                onClicked: startPageFolder.value = page.folderId
            }

            MenuItem {
                //% "New folder"
                text: qsTrId("atlantic-me-new_folder")
                visible: !page.searching
                onClicked: pageStack.animatorPush(newFolderDialog)
            }
        }

        header: Column {
            width: parent.width
            PageHeader {
                //% "Bookmarks"
                title: page.folderTitle.length > 0 ? page.folderTitle
                                                   : qsTrId("sailfish_browser-he-bookmarks")
                //% "Shown on the start page"
                description: startPageFolder.value === page.folderId
                             ? qsTrId("atlantic-la-shown_on_start_page") : ""
            }
            SearchField {
                width: parent.width
                //% "Search"
                placeholderText: qsTrId("sailfish_browser-ph-search")
                EnterKey.onClicked: focus = false
                onTextChanged: searchText = text
            }
        }

        delegate: BookmarkItem { width: listView.width }

        ViewPlaceholder {
            //% "Bookmarks that you save will show up here"
            text: qsTrId("sailfish_browser-la-bookmarks-show-up-here")
            enabled: listView.count === 0
        }

        VerticalScrollDecorator {
            parent: listView
            flickable: listView
        }
    }

    Component {
        id: newFolderDialog

        Dialog {
            property string folderName

            Column {
                width: parent.width

                DialogHeader {
                    //% "Create"
                    acceptText: qsTrId("atlantic-he-create_folder_accept")
                }

                TextField {
                    width: parent.width
                    //% "Folder name"
                    label: qsTrId("atlantic-la-folder_name")
                    placeholderText: label
                    EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                    EnterKey.onClicked: accept()
                    onTextChanged: folderName = text
                    Component.onCompleted: forceActiveFocus()
                }
            }

            canAccept: folderName.trim().length > 0
            onAccepted: page.bookmarkModel.addFolder(folderName.trim(), page.folderId)
        }
    }
}
