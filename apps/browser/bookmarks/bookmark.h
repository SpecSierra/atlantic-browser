/****************************************************************************
**
** Copyright (c) 2013 Jolla Ltd.
** Contact: Vesa-Matti Hartikainen <vesa-matti.hartikainen@jollamobile.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BOOKMARK_H
#define BOOKMARK_H

#include <QObject>

class Bookmark : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString title READ title WRITE setTitle NOTIFY titleChanged)
    Q_PROPERTY(QString url READ url WRITE setUrl NOTIFY urlChanged)
    Q_PROPERTY(QString favicon READ favicon WRITE setFavicon NOTIFY faviconChanged)

public:
    // A folder is a Bookmark with folder=true and no url; that keeps the model
    // one flat list in document order, with parentId giving the tree. id is
    // stable across saves and is what the folder UI addresses items by -- url
    // cannot be, since folders have none and the same url may be filed twice.
    // An empty id is generated; an empty parentId means the root.
    Bookmark(const QString &title, const QString &url, const QString &favicon, bool hasTouchIcon,
             const QString &id = QString(), const QString &parentId = QString(), bool folder = false,
             QObject* parent = 0);

    QString id() const { return m_id; }

    QString parentId() const { return m_parentId; }
    void setParentId(const QString &parentId);

    bool isFolder() const { return m_folder; }

    QString title() const;
    void setTitle(const QString &title);

    QString url() const;
    void setUrl(const QString &url);

    QString favicon() const;
    void setFavicon(const QString &favicon);

    bool hasTouchIcon() const;
    void setHasTouchIcon(bool hasTouchIcon);

signals:
    void titleChanged();
    void urlChanged();
    void faviconChanged();

private:
    QString m_id;
    QString m_parentId;
    bool m_folder;
    QString m_title;
    QString m_url;
    QString m_favicon;
    bool m_hasTouchIcon;
};

#endif // BOOKMARK_H
