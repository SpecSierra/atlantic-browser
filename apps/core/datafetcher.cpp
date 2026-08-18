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
        QNetworkRequest request(m_url);
        if (m_type == Favicon) {
            // Some CDNs 403 icon requests without a Referer, and a few serve
            // an HTML error page unless an image Accept is sent.
            request.setRawHeader("Accept", "image/webp,image/png,image/svg+xml,image/*,*/*;q=0.8");
            request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
        }
        QNetworkReply *reply = m_networkAccessManager.get(request);
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
        image.loadFromData(m_networkData);
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
    if (!m_networkData.isEmpty()) {
        // Let Qt sniff the format: the extension lies often enough (.ico files
        // serving PNG, .png serving SVG) that trusting it costs real icons.
        image.loadFromData(m_networkData);
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

    QUrl url = QUrl::fromLocalFile(OpenSearchConfigs::getOpenSearchConfigPath() + m_url.host() + ".xml");
    QDir dir;
    if (dir.mkpath(url.toString(QUrl::RemoveScheme | QUrl::RemoveFilename))) {
        QFile file(url.path());
        if (file.open(QIODevice::WriteOnly)) {
            if (file.write(m_networkData) > 0) {
                file.close();

                // Inform WebEngine there's a new search xml (WPE: no-op, search engine managed differently)
                updateStatus(Ready);
            } else {
                file.close();
                updateStatus(Error);
            }
        }
    }
}
