/****************************************************************************
**
** Copyright (c) 2021 Open Mobile Platform LLC.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <QFile>
#include <QTextStream>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>
#include <memory>

#include "browserpaths.h"
#include "datafetcher.h"
#include "logging.h"

#include "faviconmanager.h"

#include "WPEWebPage.h"

FaviconManager::FaviconManager(QObject *parent)
    : QObject(parent)
    , m_faviconSets()
{
}

FaviconManager *FaviconManager::instance()
{
    static FaviconManager *singleton = nullptr;
    if (!singleton) {
        singleton = new FaviconManager();
    }

    return singleton;
}

QString FaviconManager::sanitizedHostname(const QString &hostname)
{
    // Should port should be included too?
    const QUrl url(hostname);
    return QStringLiteral("%1://%2").arg(url.scheme(), url.host());
}

void FaviconManager::save(const QString &type)
{
    if (!m_faviconSets.contains(type)) {
        qCDebug(lcFavoritesLog) << "No" << type << "favicons loaded, skipping save";
        return;
    }

    const FaviconSet &faviconSet = m_faviconSets.value(type);
    if (!faviconSet.loaded) {
        qCDebug(lcFavoritesLog) << "No changes to" << type << "favicons, skipping save";
        return;
    }

    QString dataLocation = BrowserPaths::dataLocation();
    if (dataLocation.isNull()) {
        qWarning() << "No datalocation set to save" << type << "favicons to";
        return;
    }
    QString path = QString("%1/%2.json").arg(dataLocation).arg(type);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "Can't create favicons file " << path;
        return;
    }
    QTextStream out(&file);
    QJsonArray items;

    for (const QString &hostname : faviconSet.favicons.keys()) {
        const Favicon &favicon = faviconSet.favicons.value(hostname);
        QJsonObject item;
        item.insert("hostname", QJsonValue(hostname));
        item.insert("favicon", QJsonValue(favicon.favicon));
        item.insert("hasTouchIcon", QJsonValue(favicon.hasTouchIcon));
        items.append(QJsonValue(item));
    }
    QJsonDocument doc(items);
    out.setCodec("UTF-8");
    out << doc.toJson();
    file.close();
}

// After calling load it must be safe to assume the type exists in the map
void FaviconManager::load(const QString &type)
{
    // Once loaded, future calls to load return immediately
    if (m_faviconSets.contains(type) && m_faviconSets.value(type).loaded) {
        // Favicons already loaded
        return;
    }

    FaviconSet faviconSet;
    faviconSet.loaded = true;
    m_faviconSets.insert(type, faviconSet);

    QString dataLocation = BrowserPaths::dataLocation();
    if (dataLocation.isNull()) {
        qWarning() << "No datalocation set to load" << type << "favicons from";
        return;
    }
    QString path = QString("%1/%2.json").arg(dataLocation).arg(type);
    QScopedPointer<QFile> file(new QFile(path));

    if (!file->open(QIODevice::ReadOnly | QIODevice::Text)) {
        // The file may not exist yet; that's okay
        qCDebug(lcFavoritesLog) << "Unable to open favicons file " << path;
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file->readAll());
    if (doc.isArray()) {
        QJsonArray array = doc.array();
        for (const QJsonValue &value : array) {
            if (value.isObject()) {
                QJsonObject obj = value.toObject();
                QString hostname = obj.value("hostname").toString();
                Favicon favicon;
                favicon.favicon = obj.value("favicon").toString();
                favicon.hasTouchIcon = obj.value("hasTouchIcon").toBool();
                faviconSet.favicons.insert(hostname, favicon);
            }
        }
    } else {
        qWarning() << "Favicons json file should be an array of items";
    }
    file->close();

    m_faviconSets.insert(type, faviconSet);
}

void FaviconManager::add(const QString &type, const QString &hostname, const QString &favicon, bool hasTouchIcon)
{
    load(type);

    const QString host = sanitizedHostname(hostname);
    FaviconSet faviconSet = m_faviconSets.value(type);

    if (faviconSet.favicons.contains(host)) {
        const Favicon &current = faviconSet.favicons.value(host);
        if ((current.favicon == favicon) && (current.hasTouchIcon == hasTouchIcon)) {
            // No changes
            return;
        }
    }

    Favicon item;
    item.favicon = favicon;
    item.hasTouchIcon = hasTouchIcon;
    // After calling load() it's safe to assume the type exists in the map
    faviconSet.favicons.insert(host, item);
    m_faviconSets.insert(type, faviconSet);

    save(type);

    emit iconChanged(type, host, favicon);
}

void FaviconManager::remove(const QString &type, const QString &hostname)
{
    load(type);

    // After calling load() it's safe to assume the type exists in the map
    FaviconSet faviconSet = m_faviconSets.value(type);
    faviconSet.favicons.remove(sanitizedHostname(hostname));
    m_faviconSets.insert(type, faviconSet);

    save(type);
}

// Until favicon discovery existed, DataFetcher answered every lookup with the
// generic launcher icon *name* and grabIcon() stored it as if it were a real
// icon — which then made grabIcon() skip that host forever. Treat those entries
// as absent so devices upgrading from an earlier build actually pick up icons.
bool FaviconManager::isRealIcon(const QString &favicon)
{
    return !favicon.isEmpty() && favicon != defaultDesktopBookmarkIcon();
}

QString FaviconManager::get(const QString &type, const QString &hostname)
{
    load(type);

    // After calling load() it's safe to assume the type exists in the map
    QString favicon;
    QString host = sanitizedHostname(hostname);
    if (m_faviconSets.value(type).favicons.contains(host)) {
        favicon = m_faviconSets.value(type).favicons.value(host).favicon;
    }
    return isRealIcon(favicon) ? favicon : QString();
}

void FaviconManager::grabIcon(const QString &type, WPEWebPage *webPage, const QSize &size)
{
    if (!webPage)
        return;

    const QString pageUrl = webPage->url().toString();
    if (pageUrl.isEmpty())
        return;

    load(type);

    const QString host = sanitizedHostname(pageUrl);
    const Favicon stored = m_faviconSets.value(type).favicons.value(host);
    const bool haveStored = isRealIcon(stored.favicon);

    // hasTouchIcon marks a real site icon as opposed to a page thumbnail, so it
    // doubles as "nothing left to look for here".
    if (haveStored && stored.hasTouchIcon)
        return;

    if (haveStored) {
        // A thumbnail placeholder from an earlier visit (or from a version that
        // had no favicon discovery at all). Worth one upgrade attempt per
        // session, not one per page load.
        const QString key = type + QLatin1Char('|') + host;
        if (m_thumbnailUpgradeAttempted.contains(key))
            return;
        m_thumbnailUpgradeAttempted.insert(key);
    }

    // The faviconBridge user script reports a ranked list; fall back to the
    // single favicon property for anything that sets it directly.
    QStringList candidates = webPage->property("faviconCandidates").toStringList();
    if (candidates.isEmpty()) {
        const QString single = webPage->property("favicon").toString();
        if (!single.isEmpty())
            candidates.append(single);
    }

    fetchCandidate(type, pageUrl, candidates, 0, webPage, size, !haveStored);
}

// Walks the candidate list until one URL yields an image that actually decodes.
// The top-ranked candidate is regularly a 404, an HTML error page, or a format
// we cannot read, so a single-shot fetch would leave far too many sites on the
// generic icon.
void FaviconManager::fetchCandidate(const QString &type, const QString &pageUrl,
                                    const QStringList &candidates, int index,
                                    QPointer<WPEWebPage> webPage, const QSize &size,
                                    bool allowThumbnailFallback)
{
    if (index < 0 || index >= candidates.count()) {
        if (allowThumbnailFallback)
            grabThumbnailFallback(type, webPage, size);
        return;
    }

    DataFetcher *dataFetcher = new DataFetcher(this);
    dataFetcher->setType(DataFetcher::Favicon);

    std::shared_ptr<QMetaObject::Connection> dataConn = std::make_shared<QMetaObject::Connection>();
    *dataConn = connect(dataFetcher, &DataFetcher::dataChanged, this,
                        [this, dataFetcher, type, pageUrl, candidates, index, webPage, size,
                         allowThumbnailFallback, dataConn]() {
        QObject::disconnect(*dataConn);
        const QString data = dataFetcher->data();
        dataFetcher->deleteLater();

        if (isRealIcon(data)) {
            qCDebug(lcFavoritesLog) << "Storing favicon for" << type << pageUrl
                                    << "from" << candidates.at(index);
            add(type, pageUrl, data, true);
            return;
        }

        fetchCandidate(type, pageUrl, candidates, index + 1, webPage, size, allowThumbnailFallback);
    });

    dataFetcher->fetch(candidates.at(index));
}

void FaviconManager::grabThumbnailFallback(const QString &type, QPointer<WPEWebPage> webPage,
                                           const QSize &size)
{
    if (!webPage)
        return;

    std::shared_ptr<QMetaObject::Connection> thumbConn = std::make_shared<QMetaObject::Connection>();
    *thumbConn = connect(webPage.data(), &WPEWebPage::thumbnailResult, this,
                         [this, type, webPage, thumbConn](const QString &data) {
        QObject::disconnect(*thumbConn);
        if (!webPage || data.isEmpty())
            return;
        qCDebug(lcFavoritesLog) << "Storing thumbnail for" << type;
        add(type, webPage->url().toString(), data, false);
    });
    webPage->grabThumbnail(size);
}

void FaviconManager::clear(const QString &type)
{
    FaviconSet faviconSet;
    faviconSet.loaded = true;
    m_faviconSets.insert(type, faviconSet);
    save(type);
}

QString FaviconManager::defaultDesktopBookmarkIcon()
{
    return QStringLiteral("icon-launcher-bookmark");
}
