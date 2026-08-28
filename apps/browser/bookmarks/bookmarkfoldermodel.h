/****************************************************************************
**
** Copyright (c) 2026 SpecSierra
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BOOKMARKFOLDERMODEL_H
#define BOOKMARKFOLDERMODEL_H

#include <QSortFilterProxyModel>

// One folder's contents, in the source model's order. The bookmark model is a
// single flat list with a parentId on each row; this is the view of one level
// of that tree, used both by the start page (pinned to a chosen folder) and by
// the bookmarks page (following the user down as they open folders).
//
// Deliberately not a sorting proxy: the row order IS the user's chosen order,
// which is what drag-to-reorder edits.
class BookmarkFolderModel : public QSortFilterProxyModel
{
    Q_OBJECT

    // "" is the root level. A folderId that no longer resolves shows nothing,
    // rather than silently falling back to the root and looking like the
    // folder's contents escaped into it.
    Q_PROPERTY(QString folderId READ folderId WRITE setFolderId NOTIFY folderIdChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    BookmarkFolderModel(QObject *parent = nullptr);

    QString folderId() const;
    void setFolderId(const QString &folderId);

    int count() const;

    // Rows here are this model's own; they are mapped onto the source before
    // being handed to the bookmark model, whose rows are the flat list's.
    // Matches BookmarkFilterModel::getIndex, so a delegate can ask whichever
    // proxy it is under for the underlying bookmark model row.
    Q_INVOKABLE int getIndex(int row) const;
    Q_INVOKABLE void move(int from, int to);
    Q_INVOKABLE void removeAt(int index);
    // Files an item from this level into `folderId` (or "" for the root).
    Q_INVOKABLE void moveToFolder(int index, const QString &folderId);

    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    void setSourceModel(QAbstractItemModel *sourceModel) override;

signals:
    void folderIdChanged();
    void countChanged();

private:
    QString m_folderId;
};

#endif // BOOKMARKFOLDERMODEL_H
