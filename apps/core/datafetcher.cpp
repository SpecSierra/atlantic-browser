/****************************************************************************
**
** Copyright (c) 2014 - 2021 Jolla Ltd.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "datafetcher.h"
#include "faviconmanager.h"
#include "opensearchconfigs.h"

#include <QBuffer>
#include <QImage>
#include <QUrl>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QImageReader>

namespace {

// The page decides which icon URL is fetched, and it is fetched from the UI
// process (outside the ad blocker and the web content sandbox). Keep it to
// public web hosts: no loopback, link-local or private-range targets.
bool isPublicWebUrl(const QUrl &url)
{
    if (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))
        return false;
    const QString host = url.host().toLower();
    if (host.isEmpty() || host == QLatin1String("localhost") || host.endsWith(QLatin1String(".localhost"))
            || host.endsWith(QLatin1String(".local")))
        return false;
    QHostAddress address;
    if (address.setAddress(host)) {
        if (address.isLoopback() || address.isNull())
            return false;
        if (address.protocol() == QAbstractSocket::IPv4Protocol) {
            const quint32 ip = address.toIPv4Address();
            if ((ip >> 24) == 10 || (ip >> 24) == 127 || (ip >> 16) == 0xC0A8
                    || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xA9FE || (ip >> 24) == 0)
                return false;
        } else {
            const Q_IPV6ADDR v6 = address.toIPv6Address();
            if ((v6[0] & 0xfe) == 0xfc || (v6[0] == 0xfe && (v6[1] & 0xc0) == 0x80))
                return false;
        }
    }
    return true;
}

const qint64 kMaxIconBytes = 512 * 1024;
const int kMaxIconDimension = 1024;

} // namespace

DataFetcher::DataFetcher(QObject *parent)
    : QObject(parent)
    , m_status(Null)
    , m_minimumIconSize(64) // Initial value that matches theme iconSizeMedium.
    , m_hasAcceptedTouchIcon(false)
    , m_type(Icon)
{
}

void DataFetcher::fetch(const QString &url)
{
    if (m_type == Icon)
        updateAcceptedTouchIcon(false);

    m_url = url;
    QString path = m_url.path();
    updateStatus(Fetching);
    if (m_type == Favicon && url.isEmpty()) {
        // Favicon mode reports failure by leaving the data empty; the caller
        // moves on to the next candidate rather than pinning the default icon.
        m_data.clear();
        updateStatus(Error);
        emit dataChanged();
    } else if (m_type == Icon && (path.endsWith(".ico") || url.isEmpty())) {
        // Touch icons are used as launcher icons, where a 16px .ico is useless.
        // Favicon mode deliberately does not take this path: .ico is by far the
        // most common favicon format and decodes fine (libqico ships on device).
        m_data = defaultIcon();
        updateStatus(Ready);
        emit dataChanged();
    } else {
        m_networkData.clear();
        if (m_type == Favicon && !isPublicWebUrl(m_url)
                && !(m_url.scheme().startsWith(QLatin1String("http"))
                     && !m_pageUrl.host().isEmpty()
                     && m_url.host().compare(m_pageUrl.host(), Qt::CaseInsensitive) == 0)) {
            m_data.clear();
            updateStatus(Error);
            emit dataChanged();
            return;
        }
        QNetworkRequest request(m_url);
        if (m_type == Favicon) {
            // Some CDNs 403 icon requests without a Referer, and a few serve
            // an HTML error page unless an image Accept is sent.
            request.setRawHeader("Accept", "image/webp,image/png,image/svg+xml,image/*,*/*;q=0.8");
            request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
        }
        QNetworkReply *reply = m_networkAccessManager.get(request);
        if (m_type == Favicon || m_type == Icon) {
            // Icons are small; stop a hostile server from streaming us gigabytes.
            connect(reply, &QNetworkReply::downloadProgress, reply,
                    [reply](qint64 received, qint64) {
                if (received > kMaxIconBytes)
                    reply->abort();
            });
        }
        connect(reply, &QNetworkReply::finished, this, &DataFetcher::dataReady);
        // qOverload(T functionPointer) would be handy to resolve right error method but it is introduced only
        // in Qt5.7. QNetWorkReply has signal error(QNetworkReply::NetworkError) and method error().
        // connect(reply, qOverload<QNetworkReply::NetworkError>(&QNetworkReply::error), this, DataFetcher::error);
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(error(QNetworkReply::NetworkError)));
    }
}

DataFetcher::Status DataFetcher::status() const
{
    return m_status;
}

DataFetcher::Type DataFetcher::type() const
{
    return m_type;
}

void DataFetcher::setType(Type type)
{
    if (m_type != type) {
        m_type = type;
        emit typeChanged();
    }
}

QString DataFetcher::data() const
{
    return m_data;
}

QString DataFetcher::defaultIcon() const
{
    return FaviconManager::defaultDesktopBookmarkIcon();
}

bool DataFetcher::hasAcceptedTouchIcon()
{
    return m_hasAcceptedTouchIcon;
}

void DataFetcher::dataReady()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (reply) {
        m_networkData = reply->readAll();
        reply->deleteLater();
    }

    if (m_type == OpenSearch)
        saveAsSearchEngine();
    else if (m_type == Favicon)
        saveAsFavicon();
    else
        saveAsImage();
}

void DataFetcher::saveAsImage()
{
    if (m_networkData.isEmpty()) {
        m_data = defaultIcon();
    } else {
        QImage image;
        if (m_networkData.size() <= kMaxIconBytes) {
            QBuffer probe(&m_networkData);
            probe.open(QIODevice::ReadOnly);
            QImageReader reader(&probe);
            const QSize declared = reader.size();
            if (declared.isValid() ? (declared.width() <= kMaxIconDimension
                                      && declared.height() <= kMaxIconDimension)
                                   : true)
                image = reader.read();
        }
        if (image.width() < m_minimumIconSize || image.height() < m_minimumIconSize) {
            m_data = defaultIcon();
        } else {
            // TODO: use the actual image type
            m_data = QStringLiteral("data:image/png;base64,")
                    + QString::fromLatin1(m_networkData.toBase64());
        }
    }
    updateAcceptedTouchIcon(true);
    updateStatus(Ready);
    emit dataChanged();
}

// Favicons are drawn small (a history row, a suggestion, a start-page tile), so
// unlike touch icons they are normalised before storage: decoded here, scaled
// down to at most kFaviconStoreSize and re-encoded as PNG. Storing the raw
// bytes instead would put a 512x512 site icon in the favicon JSON for every
// visited host, and would keep claiming "image/png" for .ico and SVG payloads
// that QML then fails to decode.
void DataFetcher::saveAsFavicon()
{
    static const int kFaviconStoreSize = 64;

    m_data.clear();

    QImage image;
    if (!m_networkData.isEmpty() && m_networkData.size() <= kMaxIconBytes) {
        // Let Qt sniff the format: the extension lies often enough (.ico files
        // serving PNG, .png serving SVG) that trusting it costs real icons.
        // Ask for the declared size first: a tiny file can claim tens of
        // thousands of pixels per side and would be decoded into gigabytes.
        QBuffer probe(&m_networkData);
        probe.open(QIODevice::ReadOnly);
        QImageReader reader(&probe);
        const QSize declared = reader.size();
        // Formats whose handler cannot report a size up front (.ico) store
        // raw pixels, so their file size already bounds the decode.
        if (declared.isValid() ? (declared.width() <= kMaxIconDimension
                                  && declared.height() <= kMaxIconDimension)
                               : true) {
            image = reader.read();
        }
    }

    if (image.isNull()) {
        updateStatus(Error);
        emit dataChanged();
        return;
    }

    if (image.width() > kFaviconStoreSize || image.height() > kFaviconStoreSize) {
        image = image.scaled(kFaviconStoreSize, kFaviconStoreSize,
                             Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) {
        updateStatus(Error);
        emit dataChanged();
        return;
    }

    m_data = QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
    updateStatus(Ready);
    emit dataChanged();
}

void DataFetcher::error(QNetworkReply::NetworkError)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (reply) {
        reply->deleteLater();
    }

    updateStatus(Error);
    if (m_type == Icon) {
        m_data = defaultIcon();
        emit dataChanged();
    } else if (m_type == Favicon) {
        m_data.clear();
        emit dataChanged();
    }
}

void DataFetcher::updateStatus(DataFetcher::Status status)
{
    if (m_status != status) {
        m_status = status;
        emit statusChanged();
    }
}

void DataFetcher::updateAcceptedTouchIcon(bool acceptedTouchIcon)
{
    if (m_hasAcceptedTouchIcon != acceptedTouchIcon) {
        m_hasAcceptedTouchIcon = acceptedTouchIcon;
        emit hasAcceptedTouchIconChanged();
    }
}

void DataFetcher::saveAsSearchEngine()
{
    if (m_networkData.isEmpty()) {
        updateStatus(Error);
        return;
    }

    // One description per host: the file name is the key SearchEngineModel
    // recomputes when it reconciles the download with the engine's own
    // <ShortName>, so the two must agree.
    const QString directory = OpenSearchConfigs::getOpenSearchConfigPath();
    if (!QDir().mkpath(directory)) {
        updateStatus(Error);
        return;
    }

    QFile file(directory + m_url.host() + QStringLiteral(".xml"));
    if (!file.open(QIODevice::WriteOnly)) {
        updateStatus(Error);
        return;
    }

    const bool written = file.write(m_networkData) == m_networkData.size();
    file.close();
    if (!written) {
        file.remove();
        updateStatus(Error);
        return;
    }

    // Whether this is really an OpenSearch description is checked by
    // SearchEngineModel once it can parse the file.
    updateStatus(Ready);
}
