/*
 * Copyright (c) 2026
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "AdBlockListUpdater.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

namespace {

const char kDefaultBaseUrl[] = "https://specsierra.github.io/atlantic-engine/adblock";
const char kShippedDir[] = "/usr/share/atlantic-browser";
// Marker whose mtime is the last completed check (successful or "already
// current"); failures leave it untouched so the next start retries.
const char kLastCheckFile[] = "last-check";
const qint64 kCheckIntervalSecs = 7 * 24 * 3600;
const int kStartupDelayMs = 60 * 1000;

} // namespace

QString AdBlockListUpdater::cacheDir()
{
    // ~/.cache/org.sailfishos/browser/adblock — inside the firejail whitelist,
    // unlike GenericCacheLocation.
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
           + QStringLiteral("/adblock");
}

qlonglong AdBlockListUpdater::versionIn(const QString& dir)
{
    QFile f(dir + QStringLiteral("/engine.version"));
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    bool ok = false;
    const qlonglong v = QString::fromLatin1(f.readAll()).trimmed().toLongLong(&ok);
    return ok && v > 0 ? v : 0;
}

AdBlockListUpdater::AdBlockListUpdater(QObject* parent)
    : QObject(parent)
{
    const QByteArray envUrl = qgetenv("ATLANTIC_ADBLOCK_UPDATE_URL");
    m_baseUrl = envUrl.isEmpty() ? QString::fromLatin1(kDefaultBaseUrl)
                                 : QString::fromUtf8(envUrl);
}

void AdBlockListUpdater::start()
{
    if (qgetenv("ATLANTIC_ADBLOCK_UPDATE") == "0")
        return;

    const QFileInfo lastCheck(cacheDir() + QLatin1Char('/') + QLatin1String(kLastCheckFile));
    if (lastCheck.exists()
        && lastCheck.lastModified().secsTo(QDateTime::currentDateTime()) < kCheckIntervalSecs)
        return;

    // Leaked singleton; runs once per (eligible) browser start. Delayed so the
    // download never competes with startup and first page load.
    static AdBlockListUpdater* updater = nullptr;
    if (updater)
        return;
    updater = new AdBlockListUpdater();
    QTimer::singleShot(kStartupDelayMs, updater, [] { updater->checkNow(); });
}

void AdBlockListUpdater::checkNow()
{
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);

    QNetworkRequest req(QUrl(m_baseUrl + QStringLiteral("/engine.version")));
    req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] { onVersionReply(reply); });
}

void AdBlockListUpdater::onVersionReply(QNetworkReply* reply)
{
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "[ADBLOCK-UPDATE] version check failed:" << reply->errorString();
        return;
    }
    bool ok = false;
    m_remoteVersion = QString::fromLatin1(reply->readAll()).trimmed().toLongLong(&ok);
    if (!ok || m_remoteVersion <= 0) {
        qWarning() << "[ADBLOCK-UPDATE] malformed engine.version";
        return;
    }

    const qlonglong local = qMax(versionIn(QLatin1String(kShippedDir)), versionIn(cacheDir()));
    QDir().mkpath(cacheDir());
    if (m_remoteVersion <= local) {
        qInfo() << "[ADBLOCK-UPDATE] lists current (local" << local << "remote" << m_remoteVersion << ")";
        QFile marker(cacheDir() + QLatin1Char('/') + QLatin1String(kLastCheckFile));
        if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate))
            marker.write(QByteArray::number(m_remoteVersion));
        return;
    }

    qInfo() << "[ADBLOCK-UPDATE] fetching lists" << local << "->" << m_remoteVersion;
    m_pendingFiles = QStringList()
        << QStringLiteral("engine.dat")
        << QStringLiteral("adblock-resources.json");
    fetchNextFile();
}

void AdBlockListUpdater::fetchNextFile()
{
    if (m_pendingFiles.isEmpty()) {
        // Both files landed as .new — publish atomically, stamp last so a
        // partial update never looks newer than it is.
        for (const QString& name : { QStringLiteral("engine.dat"),
                                     QStringLiteral("adblock-resources.json") }) {
            const QString finalPath = cacheDir() + QLatin1Char('/') + name;
            QFile::remove(finalPath);
            if (!QFile::rename(finalPath + QStringLiteral(".new"), finalPath)) {
                qWarning() << "[ADBLOCK-UPDATE] failed to publish" << name;
                return;
            }
        }
        QSaveFile stamp(cacheDir() + QStringLiteral("/engine.version"));
        if (stamp.open(QIODevice::WriteOnly)) {
            stamp.write(QByteArray::number(m_remoteVersion));
            stamp.commit();
        }
        QFile marker(cacheDir() + QLatin1Char('/') + QLatin1String(kLastCheckFile));
        if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate))
            marker.write(QByteArray::number(m_remoteVersion));
        qInfo() << "[ADBLOCK-UPDATE] lists updated to" << m_remoteVersion
                << "- applied on next browser start";
        return;
    }

    m_currentFile = m_pendingFiles.takeFirst();
    QNetworkRequest req(QUrl(m_baseUrl + QLatin1Char('/') + m_currentFile));
    req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] { onFileReply(reply); });
}

void AdBlockListUpdater::onFileReply(QNetworkReply* reply)
{
    reply->deleteLater();
    const QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError || data.isEmpty()) {
        qWarning() << "[ADBLOCK-UPDATE]" << m_currentFile << "download failed:"
                   << reply->errorString();
        return;
    }
    QSaveFile out(cacheDir() + QLatin1Char('/') + m_currentFile + QStringLiteral(".new"));
    if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit()) {
        qWarning() << "[ADBLOCK-UPDATE] cannot write" << m_currentFile;
        return;
    }
    fetchNextFile();
}
