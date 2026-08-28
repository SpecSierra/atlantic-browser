/****************************************************************************
**
** Copyright (c) 2020 - 2021 Open Mobile Platform LLC
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Browser 1.0
import Sailfish.WebView.Popups 1.0 as Popups

ListItem {
    id: root

    // A folder opens as another bookmarks page rather than expanding in place,
    // so the pull-back gesture walks back up the tree.
    function openLink() {
        if (model.isFolder) {
            pageStack.push("../BookmarkPage.qml", {
                               bookmarkModel: bookmarkModel,
                               folderId: model.bookmarkId,
                               folderTitle: model.title
                           })
            return
        }

        webView.tabModel.newTab(model.url, true)
        pageStack.pop()
    }

    // By id, not url: folders have no url, and the same url can be filed in
    // two folders, so removing by url would take out the wrong copy.
    function remove() {
        var id = model.bookmarkId
        remorseDelete(function() {
            bookmarkModel.removeById(id)
        })
    }

    contentHeight: Math.max(Theme.itemSizeMedium, column.height + 2*Theme.paddingMedium)

    menu: contextMenuComponent

    ListView.onAdd: AddAnimation { target: root }

    FavoriteIcon {
        id: favoriteIcon

        anchors {
            verticalCenter: parent.verticalCenter
            left: parent.left
            leftMargin: Theme.horizontalPageMargin
        }

        icon: model.isFolder ? "image://theme/icon-m-folder" : model.favicon

        sourceSize.width: Theme.iconSizeMedium
        sourceSize.height: Theme.iconSizeMedium
        width: Theme.iconSizeMedium
        height: Theme.iconSizeMedium
    }

    Column {
        id: column

        anchors {
            verticalCenter: parent.verticalCenter
            left: favoriteIcon.right
            leftMargin: Theme.paddingMedium
            right: parent.right
            rightMargin: Theme.horizontalPageMargin
        }

        Label {
            text: Theme.highlightText(title, searchText, Theme.highlightColor)
            textFormat: Text.StyledText
            color: highlighted ? Theme.highlightColor : Theme.primaryColor
            font.pixelSize: Theme.fontSizeSmall
            truncationMode: TruncationMode.Fade
            width: parent.width
        }
        Label {
            visible: !model.isFolder
            text: Theme.highlightText(url, searchText, Theme.highlightColor)
            textFormat: Text.StyledText
            color: highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
            font.pixelSize: Theme.fontSizeSmall
            truncationMode: TruncationMode.Fade
            width: parent.width
        }
    }

    ListView.onRemove: animateRemoval()
    onClicked: openLink()

    Component {
        id: contextMenuComponent
        ContextMenu {
            MenuItem {
                //% "Share"
                text: qsTrId("sailfish_browser-me-share-link")
                visible: !model.isFolder
                onClicked: webShareAction.shareLink(model.url, model.title)
            }
            MenuItem {
                //% "Copy to clipboard"
                text: qsTrId("sailfish_browser-me-copy-to-clipboard")
                visible: !model.isFolder
                onClicked: Clipboard.text = model.url
            }
            MenuItem {
                text: qsTrId("sailfish_browser-me-add_to_launcher")
                visible: !model.isFolder
                onClicked: pageStack.animatorPush("AddToAppGridDialog.qml",
                                                  {
                                                      "url": url,
                                                      "title": title,
                                                      "icon": favicon,
                                                      "desktopBookmarkWriter": desktopBookmarkWriter,
                                                      "bookmarkWriterParent": pageStack
                                                  })
            }
            MenuItem {
                //% "Move to folder"
                text: qsTrId("atlantic-me-move_to_folder")
                onClicked: pageStack.animatorPush(moveToFolderDialog)
            }
            MenuItem {
                // Defined in FavoriteContextMenu.qml
                // "Edit"
                text: qsTrId("sailfish_browser-me-edit")
                visible: !model.isFolder
                onClicked: {
                    var page = pageStack.animatorPush(editDialog,
                                           {
                                               //% "Edit bookmark"
                                               "description": qsTrId("sailfish_browser-he-edit-bookmark"),
                                               "url": url,
                                               "title": title,
                                               // Whichever proxy the list is
                                               // under resolves the source row.
                                               "index": listView.model.getIndex(model.index)
                                           })
                }
            }
            MenuItem {
                //% "Rename"
                text: qsTrId("atlantic-me-rename_folder")
                visible: model.isFolder
                onClicked: pageStack.animatorPush(renameDialog)
            }
            MenuItem {
                //% "Delete"
                text: qsTrId("sailfish_browser-me-delete")
                onClicked: root.remove()
            }
        }
    }

    Component {
        id: renameDialog

        Dialog {
            property string folderName: model.title

            Column {
                width: parent.width

                DialogHeader {
                    //% "Rename"
                    acceptText: qsTrId("atlantic-he-rename_accept")
                }

                TextField {
                    width: parent.width
                    text: model.title
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
            onAccepted: bookmarkModel.rename(model.bookmarkId, folderName.trim())
        }
    }

    Component {
        id: moveToFolderDialog

        Dialog {
            // The item's own id, so a folder is never offered as a home for
            // itself. setParentId() also refuses a descendant.
            readonly property string movingId: model.bookmarkId

            Column {
                width: parent.width

                DialogHeader {
                    //% "Move"
                    acceptText: qsTrId("atlantic-he-move_accept")
                }

                BackgroundItem {
                    width: parent.width
                    onClicked: {
                        bookmarkModel.setParentId(movingId, "")
                        accept()
                    }

                    Label {
                        x: Theme.horizontalPageMargin
                        anchors.verticalCenter: parent.verticalCenter
                        //% "Bookmarks"
                        text: qsTrId("sailfish_browser-he-bookmarks")
                        color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                    }
                }

                Repeater {
                    model: bookmarkModel.folders()

                    BackgroundItem {
                        width: parent.width
                        visible: modelData.id !== movingId
                        height: visible ? Theme.itemSizeSmall : 0
                        onClicked: {
                            bookmarkModel.setParentId(movingId, modelData.id)
                            accept()
                        }

                        Label {
                            x: Theme.horizontalPageMargin
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.title
                            color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                        }
                    }
                }
            }
        }
    }

    Popups.WebShareAction {
        id: webShareAction
    }

    Component {
        id: desktopBookmarkWriter
        DesktopBookmarkWriter {
            onSaved: destroy()
        }
    }

    Component {
        id: editDialog
        BookmarkEditDialog {
            onAccepted: bookmarkModel.edit(index, editedUrl, editedTitle)
        }
    }
}
