/****************************************************************************
**
** Copyright (c) 2014 - 2021 Jolla Ltd.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "bookmarkmanager.h"
#include <QDebug>
#include <QFile>
#include <QTextStream>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>

#include "bookmark.h"
#include "browserpaths.h"

BookmarkManager::BookmarkManager()
  : QObject(nullptr)
{
}

BookmarkManager* BookmarkManager::instance()
{
    static QPointer <BookmarkManager> singleton;
    if (singleton.isNull()) {
        singleton = new BookmarkManager();
    }

    return singleton.data();
}

void BookmarkManager::save(const QList<Bookmark*> & bookmarks)
{
    QString dataLocation = BrowserPaths::dataLocation();
    if (dataLocation.isNull()) {
        return;
    }
    QString path = dataLocation + "/bookmarks.json";
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "Can't create file " << path;
        return;
    }
    QTextStream out(&file);
    QJsonArray items;

    for (const Bookmark* const bookmark : bookmarks) {
        QJsonObject title;
        title.insert("url", QJsonValue(bookmark->url()));
        title.insert("title", QJsonValue(bookmark->title()));
        title.insert("favicon", QJsonValue(bookmark->favicon()));
        title.insert("hasTouchIcon", QJsonValue(bookmark->hasTouchIcon()));
        title.insert("id", QJsonValue(bookmark->id()));
        if (!bookmark->parentId().isEmpty())
            title.insert("parentId", QJsonValue(bookmark->parentId()));
        if (bookmark->isFolder())
            title.insert("folder", QJsonValue(true));
        items.append(QJsonValue(title));
    }

    // v2 is an object so folders and ids have somewhere to live; the list order
    // inside "items" is the display order. load() still reads a bare array
    // (v1), which is both the pre-folder file and the shipped
    // default-content/bookmarks.json, so that stays supported rather than being
    // a one-shot migration.
    QJsonObject root;
    root.insert("version", QJsonValue(2));
    root.insert("items", items);
    QJsonDocument doc(root);
    out.setCodec("UTF-8");
    out << doc.toJson();
    file.close();
}

void BookmarkManager::clear()
{
    save(QList<Bookmark*>());
    emit cleared();
}

QList<Bookmark*> BookmarkManager::load() {
    QList<Bookmark*> bookmarks;
    QString bookmarkFile = BrowserPaths::dataLocation() + "/bookmarks.json";
    QScopedPointer<QFile> file(new QFile(bookmarkFile));

    if (!file->open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Unable to open bookmarks " << bookmarkFile;

        file.reset(new QFile(QLatin1Literal("/usr/share/atlantic-browser/default-content/bookmarks.json")));
        if (!file->open(QIODevice::ReadOnly | QIODevice::Text)) {
            qWarning() << "Unable to open bookmarks defaults";
            return bookmarks;
        }
    }

    QJsonDocument doc = QJsonDocument::fromJson(file->readAll());
    // v2: { "version": 2, "items": [...] }. v1: a bare array of the same item
    // objects without id/parentId/folder -- reading one just leaves every entry
    // at the root with a generated id, and the next save() writes v2.
    const QJsonArray array = doc.isObject() ? doc.object().value("items").toArray()
                                            : doc.array();
    if (doc.isArray() || doc.isObject()) {
        for (const QJsonValue &value : array) {
            if (value.isObject()) {
                QJsonObject obj = value.toObject();
                QString url = obj.value("url").toString();
                QString favicon = obj.value("favicon").toString();
                Bookmark* m = new Bookmark(obj.value("title").toString(),
                                           url,
                                           favicon,
                                           obj.value("hasTouchIcon").toBool(),
                                           obj.value("id").toString(),
                                           obj.value("parentId").toString(),
                                           obj.value("folder").toBool());
                bookmarks.append(m);
            }
        }
    } else {
        qWarning() << "bookmarks.json should be an array or a { version, items } object";
    }
    file->close();

    // Cleanup after next stop release. See JB#53083 and JB#52736
    bookmarkFile = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
            + QLatin1String("/org.atlantic/atlantic-browser/bookmarks.json");
    file.reset(new QFile(bookmarkFile));
    if (file->exists() && file->open(QIODevice::ReadOnly | QIODevice::Text)) {
        QJsonDocument doc = QJsonDocument::fromJson(file->readAll());
        if (doc.isArray()) {
            QJsonArray array = doc.array();
            for (const QJsonValue &value : array) {
                if (value.isObject()) {
                    QJsonObject obj = value.toObject();
                    QString url = obj.value("url").toString();

                    bool migrate = true;
                    for (const Bookmark *bookmark : bookmarks) {
                        if (bookmark->url() == url) {
                            migrate = false;
                            break;
                        }
                    }

                    if (migrate) {
                        Bookmark* m = new Bookmark(obj.value("title").toString(),
                                                   url,
                                                   obj.value("favicon").toString(),
                                                   obj.value("hasTouchIcon").toBool());
                        bookmarks.append(m);
                    }
                }
            }
        }
        file->close();
        file->remove();
        save(bookmarks);
    }
    // End of stop release cleanup...

    return bookmarks;
}
