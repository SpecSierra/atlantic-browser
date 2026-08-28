/****************************************************************************
**
** Copyright (c) 2026 SpecSierra
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "bookmarkfoldermodel.h"
#include "declarativebookmarkmodel.h"

BookmarkFolderModel::BookmarkFolderModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    connect(this, &QAbstractItemModel::rowsInserted, this, &BookmarkFolderModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &BookmarkFolderModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &BookmarkFolderModel::countChanged);
}

void BookmarkFolderModel::setSourceModel(QAbstractItemModel *sourceModel)
{
    if (this->sourceModel() == sourceModel)
        return;

    if (this->sourceModel())
        disconnect(this->sourceModel(), nullptr, this, nullptr);

    QSortFilterProxyModel::setSourceModel(sourceModel);

    // The filter reads parentId, which setParentId() reports as a dataChanged
    // rather than a row move -- without this the item stays visible in the
    // folder it just left until something else invalidates the filter.
    if (sourceModel) {
        connect(sourceModel, &QAbstractItemModel::dataChanged,
                this, [this](const QModelIndex &, const QModelIndex &, const QVector<int> &roles) {
            if (roles.isEmpty() || roles.contains(DeclarativeBookmarkModel::ParentIdRole))
                invalidateFilter();
        });
    }

    emit countChanged();
}

QString BookmarkFolderModel::folderId() const
{
    return m_folderId;
}

void BookmarkFolderModel::setFolderId(const QString &folderId)
{
    if (m_folderId == folderId)
        return;

    m_folderId = folderId;
    invalidateFilter();
    emit folderIdChanged();
    emit countChanged();
}

int BookmarkFolderModel::count() const
{
    return rowCount();
}

bool BookmarkFolderModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (!sourceModel())
        return false;

    const QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    return index.data(DeclarativeBookmarkModel::ParentIdRole).toString() == m_folderId;
}

int BookmarkFolderModel::getIndex(int row) const
{
    const QModelIndex sourceIndex = mapToSource(index(row, 0));
    return sourceIndex.isValid() ? sourceIndex.row() : -1;
}

void BookmarkFolderModel::move(int from, int to)
{
    DeclarativeBookmarkModel *model = qobject_cast<DeclarativeBookmarkModel *>(sourceModel());
    if (!model || from == to)
        return;

    const QModelIndex fromIndex = mapToSource(index(from, 0));
    const QModelIndex toIndex = mapToSource(index(to, 0));
    if (!fromIndex.isValid() || !toIndex.isValid())
        return;

    model->move(fromIndex.row(), toIndex.row());
}

void BookmarkFolderModel::removeAt(int index)
{
    DeclarativeBookmarkModel *model = qobject_cast<DeclarativeBookmarkModel *>(sourceModel());
    if (!model)
        return;

    const QModelIndex sourceIndex = mapToSource(this->index(index, 0));
    if (sourceIndex.isValid())
        model->remove(sourceIndex.row());
}

void BookmarkFolderModel::moveToFolder(int index, const QString &folderId)
{
    DeclarativeBookmarkModel *model = qobject_cast<DeclarativeBookmarkModel *>(sourceModel());
    if (!model)
        return;

    const QModelIndex sourceIndex = mapToSource(this->index(index, 0));
    if (sourceIndex.isValid()) {
        model->setParentId(sourceIndex.data(DeclarativeBookmarkModel::IdRole).toString(), folderId);
    }
}
