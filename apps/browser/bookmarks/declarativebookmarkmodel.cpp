/****************************************************************************
**
** Copyright (c) 2013 - 2021 Jolla Ltd.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "declarativebookmarkmodel.h"
#include "bookmarkmanager.h"
#include "faviconmanager.h"

namespace {
// First model created wins; there is one bookmark list and one UI showing it.
DeclarativeBookmarkModel *s_primaryInstance = nullptr;
}

DeclarativeBookmarkModel *DeclarativeBookmarkModel::primaryInstance()
{
    return s_primaryInstance;
}

DeclarativeBookmarkModel::~DeclarativeBookmarkModel()
{
    // A drag that ended less than the coalescing interval before shutdown must
    // still reach disk.
    if (m_saveTimer && m_saveTimer->isActive()) {
        m_saveTimer->stop();
        BookmarkManager::instance()->save(bookmarks);
    }

    if (s_primaryInstance == this)
        s_primaryInstance = nullptr;
}

DeclarativeBookmarkModel::DeclarativeBookmarkModel(QObject *parent)
    : QAbstractListModel(parent)
{
    if (!s_primaryInstance)
        s_primaryInstance = this;

    connect(BookmarkManager::instance(), &BookmarkManager::cleared,
            this, &DeclarativeBookmarkModel::clearBookmarks);
    // A bookmark is usually created before its site has ever been visited in
    // this profile, so it starts out with the generic icon. Adopt the real one
    // as soon as browsing learns it for the same host.
    connect(FaviconManager::instance(), &FaviconManager::iconChanged,
            this, &DeclarativeBookmarkModel::updateHostFavicon);
    bookmarks = BookmarkManager::instance()->load();
    rebuildIndexes();
}

void DeclarativeBookmarkModel::rebuildIndexes()
{
    bookmarkIndexes.clear();
    for (int i = 0; i < bookmarks.count(); ++i) {
        // insertMulti: the same url may legitimately appear more than once,
        // and does so more often now that it can be filed in two folders.
        bookmarkIndexes.insertMulti(bookmarks.at(i)->url(), i);
    }
}

QHash<int, QByteArray> DeclarativeBookmarkModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[UrlRole] = "url";
    roles[TitleRole] = "title";
    roles[FaviconRole] = "favicon";
    roles[TouchIconRole] = "hasTouchIcon";
    roles[IdRole] = "bookmarkId";
    roles[ParentIdRole] = "parentId";
    roles[IsFolderRole] = "isFolder";
    return roles;
}

QString DeclarativeBookmarkModel::add(const QString& url, const QString& title, const QString& favicon, bool touchIcon)
{
    Bookmark *bookmark = new Bookmark(title, url, favicon, touchIcon);
    beginInsertRows(QModelIndex(), rowCount(), rowCount());
    bookmarks.append(bookmark);
    rebuildIndexes();
    endInsertRows();
    emit countChanged();
    // Getter will check if active page is still bookmarked.
    emit activeUrlBookmarkedChanged();

    save();
    return bookmark->id();
}

void DeclarativeBookmarkModel::remove(const QString& url)
{
    if (!contains(url)) {
        return;
    }

    int index = bookmarkIndexes.value(url, -1);
    remove(index);
}

void DeclarativeBookmarkModel::remove(int index)
{
    if (index < 0 || index >= bookmarks.count())
        return;

    // Deleting a folder deletes what is filed in it; leaving the children
    // behind would strand them under a parentId that no longer resolves, and
    // nothing in the UI can reach an item in that state.
    if (bookmarks.at(index)->isFolder()) {
        const QString folderId = bookmarks.at(index)->id();
        for (int i = bookmarks.count() - 1; i >= 0; --i) {
            if (bookmarks.at(i)->parentId() == folderId)
                removeAt(i);
        }
        // The folder's own row may have shifted while its children went.
        index = indexOfId(folderId);
        if (index < 0)
            return;
    }

    removeAt(index);

    emit countChanged();
    // Getter will check if active page is still bookmarked.
    emit activeUrlBookmarkedChanged();
    save();
}

// Drops one row without saving or signalling count/bookmarked changes, so a
// folder delete can take its children out in the same pass.
void DeclarativeBookmarkModel::removeAt(int index)
{
    if (index < 0 || index >= bookmarks.count())
        return;

    beginRemoveRows(QModelIndex(), index, index);
    delete bookmarks.takeAt(index);
    rebuildIndexes();
    endRemoveRows();
}

QString DeclarativeBookmarkModel::addFolder(const QString &title, const QString &parentId)
{
    Bookmark *folder = new Bookmark(title, QString(), QString(), false,
                                    QString(), parentId, true);
    beginInsertRows(QModelIndex(), rowCount(), rowCount());
    bookmarks.append(folder);
    rebuildIndexes();
    endInsertRows();
    emit countChanged();
    save();
    return folder->id();
}

int DeclarativeBookmarkModel::indexOfId(const QString &id) const
{
    for (int i = 0; i < bookmarks.count(); ++i) {
        if (bookmarks.at(i)->id() == id)
            return i;
    }
    return -1;
}

void DeclarativeBookmarkModel::removeById(const QString &id)
{
    remove(indexOfId(id));
}

void DeclarativeBookmarkModel::rename(const QString &id, const QString &title)
{
    const int row = indexOfId(id);
    if (row < 0 || bookmarks.at(row)->title() == title)
        return;

    bookmarks.at(row)->setTitle(title);
    const QModelIndex modelIndex = QAbstractListModel::index(row);
    emit dataChanged(modelIndex, modelIndex, QVector<int>() << TitleRole);
    save();
}

void DeclarativeBookmarkModel::setParentId(const QString &id, const QString &parentId)
{
    const int row = indexOfId(id);
    if (row < 0 || bookmarks.at(row)->parentId() == parentId)
        return;

    // A folder cannot be filed into itself or into its own descendant; that
    // would detach the whole branch from the root, where nothing can reach it.
    if (bookmarks.at(row)->isFolder()) {
        QString walk = parentId;
        while (!walk.isEmpty()) {
            if (walk == id)
                return;
            const int parentRow = indexOfId(walk);
            if (parentRow < 0)
                break;
            walk = bookmarks.at(parentRow)->parentId();
        }
    }

    bookmarks.at(row)->setParentId(parentId);
    const QModelIndex modelIndex = QAbstractListModel::index(row);
    emit dataChanged(modelIndex, modelIndex, QVector<int>() << ParentIdRole);
    save();
}

void DeclarativeBookmarkModel::move(int from, int to)
{
    if (from == to
            || from < 0 || from >= bookmarks.count()
            || to < 0 || to >= bookmarks.count()) {
        return;
    }

    // beginMoveRows wants the destination in pre-move coordinates: moving down
    // means the row lands after the one currently at `to`.
    if (!beginMoveRows(QModelIndex(), from, from, QModelIndex(), to > from ? to + 1 : to))
        return;
    bookmarks.move(from, to);
    rebuildIndexes();
    endMoveRows();
    saveSoon();
}

QString DeclarativeBookmarkModel::folderTitle(const QString &id) const
{
    const int row = indexOfId(id);
    if (row < 0 || !bookmarks.at(row)->isFolder())
        return QString();
    return bookmarks.at(row)->title();
}

QVariantList DeclarativeBookmarkModel::folders() const
{
    QVariantList result;
    for (const Bookmark* const bookmark : bookmarks) {
        if (!bookmark->isFolder())
            continue;
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), bookmark->id());
        entry.insert(QStringLiteral("title"), bookmark->title());
        entry.insert(QStringLiteral("parentId"), bookmark->parentId());
        result.append(entry);
    }
    return result;
}

void DeclarativeBookmarkModel::updateFavoriteIcon(const QString &url, const QString &favicon, bool touchIcon)
{
    int bookmarkIndex = bookmarkIndexes.value(url, -1);
    if (bookmarkIndex >= 0) {
        Bookmark *bookmark = bookmarks[bookmarkIndex];
        QVector<int> roles;
        if (bookmark->favicon() != favicon) {
            roles << FaviconRole;
            bookmark->setFavicon(favicon);
        }
        if (bookmark->hasTouchIcon() != touchIcon) {
            roles << TouchIconRole;
            bookmark->setHasTouchIcon(touchIcon);
        }
        if (roles.count() > 0) {
            emit dataChanged(index(bookmarkIndex), index(bookmarkIndex), roles);
            save();
        }
    }
}

// Bookmark::Bookmark() substitutes the generic launcher icon when no icon was
// captured — a bookmark is usually created before its site has been visited in
// this profile. Returns the icon browsing has since learned for the same host,
// or an empty string when the bookmark already carries an icon of its own.
QString DeclarativeBookmarkModel::learnedFavicon(const Bookmark *bookmark)
{
    const QString favicon = bookmark->favicon();
    if (!favicon.isEmpty() && favicon != FaviconManager::defaultDesktopBookmarkIcon())
        return QString();

    return FaviconManager::instance()->get(QStringLiteral("history"), bookmark->url());
}

void DeclarativeBookmarkModel::updateHostFavicon(const QString &type, const QString &hostname,
                                                 const QString &favicon)
{
    Q_UNUSED(favicon);

    if (type != QStringLiteral("history"))
        return;

    const QVector<int> roles { FaviconRole, TouchIconRole };
    for (int i = 0; i < bookmarks.count(); ++i) {
        const Bookmark *bookmark = bookmarks.at(i);
        if (FaviconManager::sanitizedHostname(bookmark->url()) != hostname)
            continue;
        if (learnedFavicon(bookmark).isEmpty())
            continue;
        emit dataChanged(index(i), index(i), roles);
    }
}

void DeclarativeBookmarkModel::edit(int index, const QString& url, const QString& title)
{
    if (index < 0 || index >= bookmarks.count())
        return;

    Bookmark * bookmark = bookmarks.value(index);
    QVector<int> roles;
    if (url != bookmark->url()) {
        bookmark->setUrl(url);
        roles << UrlRole;

        rebuildIndexes();

        // Getter will check if active page is still bookmarked.
        emit activeUrlBookmarkedChanged();
    }
    if (title != bookmark->title()) {
        bookmark->setTitle(title);
        roles << TitleRole;
    }
    if (roles.count() > 0) {
        QModelIndex modelIndex = QAbstractListModel::index(index);
        emit dataChanged(modelIndex, modelIndex, roles);
        save();
    }
}

QString DeclarativeBookmarkModel::activeUrl() const
{
    return m_activeUrl;
}

void DeclarativeBookmarkModel::setActiveUrl(const QString &url)
{
    if (m_activeUrl != url) {
        m_activeUrl = url;
        // Getter will check if active page is still bookmarked.
        emit activeUrlBookmarkedChanged();
        emit activeUrlChanged();
    }
}

bool DeclarativeBookmarkModel::activeUrlBookmarked() const
{
    return contains(m_activeUrl);
}

void DeclarativeBookmarkModel::clearBookmarks()
{
    beginRemoveRows(QModelIndex(), 0, qMax<int>(0, bookmarks.count()-1));
    bookmarks.clear();
    bookmarkIndexes.clear();
    endRemoveRows();
    emit countChanged();
}

void DeclarativeBookmarkModel::save()
{
    flushPendingSave();
    BookmarkManager::instance()->save(bookmarks);
}

void DeclarativeBookmarkModel::saveSoon()
{
    if (!m_saveTimer) {
        m_saveTimer = new QTimer(this);
        m_saveTimer->setSingleShot(true);
        m_saveTimer->setInterval(400);
        connect(m_saveTimer, &QTimer::timeout, this, [this]() {
            BookmarkManager::instance()->save(bookmarks);
        });
    }
    m_saveTimer->start();
}

void DeclarativeBookmarkModel::flushPendingSave()
{
    if (m_saveTimer && m_saveTimer->isActive())
        m_saveTimer->stop();
}

int DeclarativeBookmarkModel::rowCount(const QModelIndex & parent) const
{
    Q_UNUSED(parent)
    return bookmarks.count();
}

QVariant DeclarativeBookmarkModel::data(const QModelIndex & index, int role) const
{
    if (index.row() < 0 || index.row() >= bookmarks.count())
        return QVariant();

    const Bookmark * bookmark = bookmarks.value(index.row());
    if (role == UrlRole) {
        return bookmark->url();
    } else if (role == TitleRole) {
        return bookmark->title();
    } else if (role == FaviconRole) {
        const QString learned = learnedFavicon(bookmark);
        return learned.isEmpty() ? bookmark->favicon() : learned;
    } else if (role == TouchIconRole) {
        // A learned favicon is a real site icon, not a page thumbnail, so it
        // must not get the thumbnail mask FavoriteItem applies otherwise.
        return bookmark->hasTouchIcon() || !learnedFavicon(bookmark).isEmpty();
    } else if (role == IdRole) {
        return bookmark->id();
    } else if (role == ParentIdRole) {
        return bookmark->parentId();
    } else if (role == IsFolderRole) {
        return bookmark->isFolder();
    }
    return QVariant();
}

bool DeclarativeBookmarkModel::contains(const QString& url) const
{
    return bookmarkIndexes.contains(url);
}
