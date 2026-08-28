/****************************************************************************
**
** Copyright (c) 2013 Jolla Ltd.
** Contact: Vesa-Matti Hartikainen <vesa-matti.hartikainen@jollamobile.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DECLARATIVEBOOKMARKMODEL_H
#define DECLARATIVEBOOKMARKMODEL_H

#include <QAbstractListModel>
#include <QStringList>
#include <QMap>
#include <QVariantList>
#include <QTimer>

#include "bookmark.h"

class DeclarativeBookmarkModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged FINAL)
    Q_PROPERTY(QString activeUrl READ activeUrl WRITE setActiveUrl NOTIFY activeUrlChanged FINAL)
    Q_PROPERTY(bool activeUrlBookmarked READ activeUrlBookmarked NOTIFY activeUrlBookmarkedChanged FINAL)
public:
    DeclarativeBookmarkModel(QObject *parent = 0);
    ~DeclarativeBookmarkModel() override;

    // The model the UI is showing. browser.bookmarks writes through it rather
    // than saving bookmarks.json behind the live list's back, which would be
    // overwritten by the next UI-side save. Null until QML has created one.
    static DeclarativeBookmarkModel *primaryInstance();

    enum BookmarkRoles {
           UrlRole = Qt::UserRole + 1,
           TitleRole,
           FaviconRole,
           TouchIconRole,
           IdRole,
           ParentIdRole,
           IsFolderRole,
    };

    // Returns the new bookmark's id so a caller that added it from inside a
    // folder can file it there; add() itself always appends at the root.
    Q_INVOKABLE QString add(const QString& url, const QString& title, const QString& favicon, bool touchIcon = false);
    Q_INVOKABLE void remove(const QString& url);
    Q_INVOKABLE void remove(int index);
    Q_INVOKABLE void updateFavoriteIcon(const QString& url, const QString& favicon, bool touchIcon);
    Q_INVOKABLE bool contains(const QString& url) const;
    Q_INVOKABLE void edit(int index, const QString& url, const QString& title);

    // Folder / ordering API. Items live in one flat list whose order is the
    // display order; parentId gives the tree. Rows are addressed by id here
    // because a url identifies nothing once folders exist -- folders have no
    // url, and the same url can be filed in two places.
    Q_INVOKABLE QString addFolder(const QString &title, const QString &parentId = QString());
    Q_INVOKABLE void removeById(const QString &id);
    Q_INVOKABLE void rename(const QString &id, const QString &title);
    Q_INVOKABLE void setParentId(const QString &id, const QString &parentId);
    // Both are source-model rows. BookmarkFolderModel maps its own filtered
    // rows onto these.
    Q_INVOKABLE void move(int from, int to);
    Q_INVOKABLE int indexOfId(const QString &id) const;
    // "" when the id is not a folder, so QML can resolve a stored folder id
    // back to a title (and notice when the folder has been deleted).
    Q_INVOKABLE QString folderTitle(const QString &id) const;
    // {id, title} for every folder, for the "move to folder" pickers.
    Q_INVOKABLE QVariantList folders() const;
    // -1 when the url is not bookmarked.
    int indexOfUrl(const QString &url) const { return bookmarkIndexes.value(url, -1); }

    QString activeUrl() const;
    void setActiveUrl(const QString& url);

    bool activeUrlBookmarked() const;

    // From QAbstractListModel
    int rowCount(const QModelIndex & parent = QModelIndex()) const override;
    QVariant data(const QModelIndex & index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

private:
    static QString learnedFavicon(const Bookmark *bookmark);
    // The url -> row map was maintained by hand on every mutation, with a
    // different fix-up loop per operation. Reordering and reparenting would
    // need two more, so it is rebuilt wholesale instead -- the list is short
    // and this cannot drift out of sync.
    void rebuildIndexes();
    void removeAt(int index);

private slots:
    void clearBookmarks();
    void updateHostFavicon(const QString &type, const QString &hostname, const QString &favicon);

signals:
    void countChanged();
    void activeUrlChanged();
    void activeUrlBookmarkedChanged();

private:
    void save();
    // move() runs once per slot crossed while a tile is dragged, and each one
    // used to rewrite bookmarks.json. Reordering is coalesced onto a timer;
    // everything else still writes immediately, so only the drag can lose the
    // last few hundred ms if the process dies mid-gesture.
    void saveSoon();
    void flushPendingSave();

    QTimer *m_saveTimer = nullptr;

    QString m_activeUrl;

    QList<Bookmark*> bookmarks;
    // This map accelerates access to the `bookmarks` list's elements by their URL.
    // Consider this as an analog of a DB index for `bookmarks` table indexed by URLs.
    QMap<QString, int> bookmarkIndexes;
};
#endif // DECLARATIVEBOOKMARKMODEL_H
