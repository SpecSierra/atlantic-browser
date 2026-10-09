/****************************************************************************
**
** Copyright (c) 2015 Jolla Ltd.
** Contact: Dmitry Rozhkov <dmitry.rozhkov@jolla.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <QDebug>
#include <QString>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include "browserpaths.h"

#include <pwd.h>
#include <grp.h>
#include <unistd.h>

static const char *const kDatabaseName = "atlantic-browser.sqlite";

static QString getLocation(QStandardPaths::StandardLocation locationType)
{
    QString location(QStandardPaths::writableLocation(locationType));
    QDir dir(location);
    if (!dir.exists()) {
        if (!dir.mkpath(location)) {
            qWarning() << QString("Can't create directory %1").arg(location);
            return QString();
        }
    }

    return location;
}

QString BrowserPaths::downloadLocation()
{
    return getLocation(QStandardPaths::DownloadLocation);
}

QString BrowserPaths::picturesLocation()
{
    return getLocation(QStandardPaths::PicturesLocation);
}

QString BrowserPaths::dataLocation()
{
    return getLocation(QStandardPaths::AppDataLocation);
}

QString BrowserPaths::applicationsLocation()
{
    return getLocation(QStandardPaths::ApplicationsLocation);
}

QString BrowserPaths::cacheLocation()
{
    return getLocation(QStandardPaths::CacheLocation);
}

QString BrowserPaths::databasePath()
{
    QString databaseDir = BrowserPaths::dataLocation();
    if (databaseDir.isNull()) {
        qWarning() << "Unable to get database dir";
        return QString();
    }

    QDir dir(databaseDir);
    const QString dbFileName = QLatin1String(kDatabaseName);
    return dir.absoluteFilePath(dbFileName);
}

// --- migration out of the stock browser's profile ---------------------------
//
// Up to 1.6.x Atlantic called itself org.sailfishos/browser, so everything it
// persisted sat in the stock sailfish-browser's profile next to that browser's
// own files: clearing bookmarks here emptied the stock browser's, and the
// sandbox had to expose the stock profile (cookies, saved passwords) to
// Atlantic's processes. Atlantic now has org.atlantic/atlanticbrowser to
// itself; this carries an existing user's data over on the first run.

static const char *const kLegacyProfile = "/org.sailfishos/browser";
static const char *const kMigratedMarker = "/.shared-profile-migrated";

// Both helpers leave an existing destination alone: a second pass must not put
// stale data over what the browser has written since.
static bool copyEntry(const QString &from, const QString &to)
{
    const QFileInfo source(from);
    if (!source.exists() || QFileInfo::exists(to))
        return true;

    if (!source.isDir())
        return QFile::copy(from, to);

    if (!QDir().mkpath(to))
        return false;
    bool ok = true;
    const QStringList entries = QDir(from).entryList(
                QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QString &entry : entries)
        ok = copyEntry(from + QLatin1Char('/') + entry, to + QLatin1Char('/') + entry) && ok;
    return ok;
}

static bool moveEntry(const QString &from, const QString &to)
{
    const QFileInfo source(from);
    if (!source.exists() || QFileInfo::exists(to))
        return true;

    // QFile::rename falls back to copy + remove. That matters: inside the jail
    // the two directories are separate bind mounts, so rename(2) is EXDEV even
    // though they are on one filesystem.
    if (!source.isDir())
        return QFile::rename(from, to);

    if (QDir().rename(from, to))
        return true;
    if (!copyEntry(from, to))
        return false;
    return QDir(from).removeRecursively();
}

QString BrowserPaths::legacyDataLocation()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
            + QLatin1String(kLegacyProfile);
}

void BrowserPaths::migrateSharedProfile()
{
    const QString data = dataLocation();
    if (data.isNull() || QFile::exists(data + QLatin1String(kMigratedMarker)))
        return;

    const QString legacyData = legacyDataLocation();
    const QString database = QString(QLatin1Char('/')) + QLatin1String(kDatabaseName);
    // Only someone who has run Atlantic from the old directory has anything to
    // carry over. The database says so, wherever a previous pass left it.
    if (!QFile::exists(legacyData + database) && !QFile::exists(data + database)) {
        QFile(data + QLatin1String(kMigratedMarker)).open(QIODevice::WriteOnly);
        return;
    }

    bool ok = true;
    auto dataEntry = [&](bool (*transfer)(const QString &, const QString &), const QString &name) {
        const QString entry = QString(QLatin1Char('/')) + name;
        if (!transfer(legacyData + entry, data + entry)) {
            qWarning() << "Profile migration: could not carry over" << name;
            ok = false;
        }
    };

    // Names the stock browser uses too (its favicon sets, its search engines,
    // its first-use flag): take a copy and leave its own alone. bookmarks.json
    // is one of these as well; BookmarkManager imports it.
    for (const char *name : { "history.json", "logins.json", "searchEngines", ".firstUseDone" })
        dataEntry(copyEntry, QLatin1String(name));

    // Atlantic's alone.
    for (const char *name : { "atlantic-browser.sqlite", "cookies.sqlite", "logins.db" }) {
        for (const char *suffix : { "-wal", "-shm", "-journal", "" })
            dataEntry(moveEntry, QString::fromLatin1(name) + QLatin1String(suffix));
    }
    for (const char *name : { "extensions", "extension-data" })
        dataEntry(moveEntry, QLatin1String(name));

    const QString legacyCache =
            QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
            + QLatin1String(kLegacyProfile);
    const QString cache = cacheLocation();
    if (!cache.isNull()) {
        // The downloaded filter lists are worth keeping; the rest is
        // regenerated (tab thumbnails are stored by absolute path, so the old
        // files would be orphans either way).
        if (!moveEntry(legacyCache + QLatin1String("/adblock"), cache + QLatin1String("/adblock"))) {
            qWarning() << "Profile migration: could not carry over the adblock lists";
            ok = false;
        }
        for (const char *name : { "/thumbnails", "/history-preview", "/extension-downloads" })
            QDir(legacyCache + QLatin1String(name)).removeRecursively();
    }

    // A failed entry is retried on the next start; whatever did arrive is
    // skipped then, since neither helper touches an existing destination.
    if (ok)
        QFile(data + QLatin1String(kMigratedMarker)).open(QIODevice::WriteOnly);
}

bool BrowserPaths::createDirectory(const QString &dirStr)
{
    QDir dir(dirStr);
    if (!dir.exists()) {
        if (!dir.mkpath(dirStr)) {
            return false;
        }
        uid_t uid = getuid();
        // assumes that correct groupname is same as username
        int gid = getgrnam(getpwuid(uid)->pw_name)->gr_gid;
        int success = chown(dirStr.toLatin1().data(), uid, gid);
        Q_UNUSED(success);
        QFile::Permissions permissions(QFile::ExeOwner
                                       | QFile::ExeGroup
                                       | QFile::ReadOwner
                                       | QFile::WriteOwner
                                       | QFile::ReadGroup
                                       | QFile::WriteGroup);
        QFile::setPermissions(dirStr, permissions);
    }
    return true;
}
