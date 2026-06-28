/*
 * Copyright (c) 2013 - 2021 Jolla Ltd.
 * SPDX-License-Identifier: MPL-2.0
 *
 * WPE replacement: Gecko download management removed.
 * Downloads are handled via WebKit download delegates (future work).
 */

#include "downloadmanager.h"
#include "browserpaths.h"
#include "logging.h"

#include <transferengineinterface.h>
#include <transfertypes.h>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QDebug>
#include <algorithm>
#include <wpe/webkit.h>

static DownloadManager *gSingleton = nullptr;

namespace {

constexpr auto kBrowserServiceName = "org.atlantic.browser";
constexpr auto kBrowserObjectPath = "/";
constexpr auto kBrowserInterface = "org.atlantic.browser";
constexpr auto kTransferCancelMethod = "cancelTransfer";
constexpr auto kTransferRestartMethod = "restartTransfer";
constexpr auto kBrowserIconName = "icon-launcher-browser";

QString sanitizeFileName(QString fileName)
{
    fileName = QFileInfo(fileName).fileName();
    if (fileName.isEmpty())
        return QStringLiteral("download");
    return fileName;
}

} // namespace

DownloadManager::DownloadManager()
    : QObject(),
      m_pdfPrinting(false)
{
    m_transferClient = new TransferEngineInterface("org.nemo.transferengine",
                                                   "/org/nemo/transferengine",
                                                   QDBusConnection::sessionBus(),
                                                   this);

    // Ignore the download info argument of the downloadStatusChanged signal.
    connect(this, &DownloadManager::downloadStatusChanged, [=](int downloadId, int status) {
        m_statusCache.insert(downloadId, static_cast<DownloadStatus::Status>(status));
    });
}

DownloadManager::~DownloadManager()
{
    gSingleton = nullptr;
}

void DownloadManager::cancelActiveTransfers()
{
    for (qulonglong downloadId : m_statusCache.keys()) {
        if (m_statusCache.value(downloadId) == DownloadStatus::Started) {
            cancelTransfer(m_download2transferMap.value(downloadId));
        }
    }
}

void DownloadManager::cancel(int downloadId)
{
    if (!m_downloadIdToObject.contains(downloadId))
        return;

    if (WebKitDownload *download = m_downloadIdToObject.value(downloadId, nullptr)) {
        webkit_download_cancel(download);
    }

    finalizeDownload(downloadId,
                     DownloadStatus::Canceled,
                     TransferEngineData::TransferCanceled,
                     QStringLiteral("Canceled by user"));
}

void DownloadManager::cancelTransfer(int transferId)
{
    if (m_transfer2downloadMap.contains(transferId)) {
        cancel(m_transfer2downloadMap.value(transferId));
    } else {
        m_transferClient->finishTransfer(transferId,
                                         TransferEngineData::TransferInterrupted,
                                         QString("Transfer got unavailable"));
    }
}

void DownloadManager::restartTransfer(int transferId)
{
    if (m_transfer2downloadMap.contains(transferId)) {
        // WebKit download restart is not directly supported; report interruption so
        // the stale transfer entry is cleaned up instead of silently doing nothing.
        m_transferClient->finishTransfer(transferId,
                                         TransferEngineData::TransferInterrupted,
                                         QString("Restart not supported for active WebKit download"));
    } else {
        m_transferClient->finishTransfer(transferId,
                                         TransferEngineData::TransferInterrupted,
                                         QString("Transfer got unavailable"));
    }
}

void DownloadManager::setPdfPrinting(const bool pdfPrinting)
{
    if (m_pdfPrinting != pdfPrinting) {
        m_pdfPrinting = pdfPrinting;
        emit pdfPrintingChanged();
    }
}

DownloadManager *DownloadManager::instance()
{
    if (!gSingleton) {
        gSingleton = new DownloadManager();
    }
    return gSingleton;
}

bool DownloadManager::existActiveTransfers()
{
    for (DownloadStatus::Status status : m_statusCache) {
        if (status == DownloadStatus::Started) return true;
    }
    return false;
}

bool DownloadManager::pdfPrinting() const
{
    return m_pdfPrinting;
}

bool DownloadManager::prepareDownload(WebKitDownload *download, const QString &suggestedFilename)
{
    if (!download)
        return false;

    // Async destination handling (WebKit >= 2.40): we return TRUE here without
    // setting a destination, which tells WebKit to hold the download until we
    // call webkit_download_set_destination() (confirmDownload) or
    // webkit_download_cancel() (cancelPendingDownload). This lets the UI prompt
    // the user for a "Save As" location before any data is written.
    const int downloadId = ensureDownloadId(download);

    // The response is already available at decide-destination time; cache the
    // metadata now so the transfer entry can be created once the user confirms.
    WebKitURIResponse *response = webkit_download_get_response(download);
    const gchar *mimeType = response ? webkit_uri_response_get_mime_type(response) : nullptr;
    const qlonglong expectedSize = response ? static_cast<qlonglong>(webkit_uri_response_get_content_length(response)) : 0;

    QVariantMap info;
    info.insert(QStringLiteral("mimeType"), QString::fromUtf8(mimeType ? mimeType : ""));
    info.insert(QStringLiteral("expectedSize"), QVariant::fromValue(expectedSize));
    m_downloadInfoCache.insert(downloadId, info);

    const QString safeName = sanitizeFileName(suggestedFilename);
    const QString defaultDir = BrowserPaths::downloadLocation();

    emit saveAsRequested(downloadId, safeName, defaultDir);
    return true;
}

void DownloadManager::confirmDownload(int downloadId, const QString &destinationPath)
{
    WebKitDownload *download = m_downloadIdToObject.value(downloadId, nullptr);
    if (!download || destinationPath.isEmpty())
        return;

    // Make sure the chosen directory exists before handing the path to WebKit.
    BrowserPaths::createDirectory(QFileInfo(destinationPath).absolutePath());

    const QByteArray destinationUri = QUrl::fromLocalFile(destinationPath).toEncoded();
    webkit_download_set_destination(download, destinationUri.constData());
    // The user explicitly chose this path, so honour it even if it exists.
    webkit_download_set_allow_overwrite(download, TRUE);

    const QString displayName = QFileInfo(destinationPath).fileName();
    QVariantMap info = m_downloadInfoCache.value(downloadId);
    info.insert(QStringLiteral("displayName"), displayName);
    info.insert(QStringLiteral("path"), destinationPath);
    m_downloadInfoCache.insert(downloadId, info);

    const qlonglong expectedSize = info.value(QStringLiteral("expectedSize")).toLongLong();

    emit downloadStarted();
    emit downloadStatusChanged(downloadId, DownloadStatus::Started, info);

    const QStringList callback{
        QLatin1String(kBrowserServiceName),
        QLatin1String(kBrowserObjectPath),
        QLatin1String(kBrowserInterface)
    };
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(
        m_transferClient->createDownload(
            displayName,
            QLatin1String(kBrowserIconName),
            QLatin1String(kBrowserIconName),
            destinationPath,
            info.value(QStringLiteral("mimeType")).toString(),
            expectedSize,
            callback,
            QLatin1String(kTransferCancelMethod),
            QLatin1String(kTransferRestartMethod)),
        this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, downloadId](QDBusPendingCallWatcher *self) {
                QDBusPendingReply<int> reply = *self;
                if (reply.isError()) {
                    qCWarning(lcDownloadLog) << "createDownload failed for" << downloadId << reply.error().message();
                    // No transfer will ever exist for this download; drop the
                    // orphaned bookkeeping so it doesn't leak or block teardown.
                    releaseDownload(downloadId);
                    checkAllTransfers();
                    self->deleteLater();
                    return;
                }

                const int transferId = reply.value();
                m_download2transferMap.insert(downloadId, transferId);
                m_transfer2downloadMap.insert(transferId, downloadId);
                m_transferClient->startTransfer(transferId);

                // The WebKit download can finish (or report progress) before this
                // asynchronous createDownload reply arrives — fast downloads do so
                // routinely. Flush whatever happened in the meantime now that we
                // finally have a transfer id; otherwise the transfer would stay
                // stuck at "Downloading" forever.
                if (m_pendingFinal.contains(downloadId)) {
                    const PendingFinal pending = m_pendingFinal.take(downloadId);
                    m_transferClient->finishTransfer(transferId, pending.transferStatus, pending.reason);
                    m_transfer2downloadMap.remove(transferId);
                    m_download2transferMap.remove(downloadId);
                    releaseDownload(downloadId);
                    checkAllTransfers();
                } else if (m_pendingProgress.contains(downloadId)) {
                    m_transferClient->updateTransferProgress(transferId, m_pendingProgress.take(downloadId));
                }

                self->deleteLater();
            });
}

void DownloadManager::cancelPendingDownload(int downloadId)
{
    // The user dismissed the "Save As" prompt before a destination was set, so
    // no transfer entry exists yet — just cancel the WebKit download and drop
    // the bookkeeping.
    if (WebKitDownload *download = m_downloadIdToObject.value(downloadId, nullptr))
        webkit_download_cancel(download);

    releaseDownload(downloadId);
}

void DownloadManager::updateDownload(WebKitDownload *download)
{
    if (!download || !m_downloadObjectToId.contains(download))
        return;

    const int downloadId = m_downloadObjectToId.value(download);
    const double progress = std::clamp(webkit_download_get_estimated_progress(download), 0.0, 1.0);

    const int transferId = transferIdForDownload(downloadId);
    if (transferId <= 0) {
        // createDownload reply not back yet; remember the latest progress so the
        // watcher can flush it once the transfer id is assigned.
        m_pendingProgress.insert(downloadId, progress);
        return;
    }
    m_transferClient->updateTransferProgress(transferId, progress);
}

void DownloadManager::downloadFinished(WebKitDownload *download)
{
    if (!download || !m_downloadObjectToId.contains(download))
        return;

    const int downloadId = m_downloadObjectToId.value(download);
    finalizeDownload(downloadId,
                     DownloadStatus::Done,
                     TransferEngineData::TransferFinished,
                     QString());
}

void DownloadManager::downloadFailed(WebKitDownload *download, const QString &reason)
{
    if (!download || !m_downloadObjectToId.contains(download))
        return;

    const int downloadId = m_downloadObjectToId.value(download);
    finalizeDownload(downloadId,
                     DownloadStatus::Failed,
                     TransferEngineData::TransferInterrupted,
                     reason);
}

int DownloadManager::ensureDownloadId(WebKitDownload *download)
{
    if (m_downloadObjectToId.contains(download))
        return m_downloadObjectToId.value(download);

    const int downloadId = m_nextDownloadId++;
    g_object_ref(download);
    m_downloadObjectToId.insert(download, downloadId);
    m_downloadIdToObject.insert(downloadId, download);
    return downloadId;
}

QString DownloadManager::ensureDestinationPath(const QString &suggestedFilename) const
{
    const QString baseDir = BrowserPaths::downloadLocation();
    if (!BrowserPaths::createDirectory(baseDir))
        return QString();

    const QString safeName = sanitizeFileName(suggestedFilename);
    const QFileInfo info(safeName);
    const QString stem = info.completeBaseName().isEmpty() ? QStringLiteral("download") : info.completeBaseName();
    const QString suffix = info.suffix();

    QString candidate = QDir(baseDir).filePath(safeName);
    if (!QFileInfo::exists(candidate))
        return candidate;

    for (int index = 1; index < 10000; ++index) {
        const QString numberedName = suffix.isEmpty()
            ? QStringLiteral("%1 (%2)").arg(stem).arg(index)
            : QStringLiteral("%1 (%2).%3").arg(stem).arg(index).arg(suffix);
        candidate = QDir(baseDir).filePath(numberedName);
        if (!QFileInfo::exists(candidate))
            return candidate;
    }

    return QString();
}

int DownloadManager::transferIdForDownload(int downloadId) const
{
    return m_download2transferMap.value(downloadId, -1);
}

QVariantMap DownloadManager::downloadInfo(int downloadId) const
{
    return m_downloadInfoCache.value(downloadId);
}

void DownloadManager::finalizeDownload(int downloadId, DownloadStatus::Status status, int transferStatus, const QString &reason)
{
    emit downloadStatusChanged(downloadId, status, downloadInfo(downloadId));

    const int transferId = transferIdForDownload(downloadId);
    if (transferId <= 0) {
        // The asynchronous createDownload reply has not arrived yet, so there is
        // no transfer id to finish against. Defer the completion; the
        // createDownload watcher flushes it (and releases the download) once the
        // id is known. Without this, a download that finishes before the D-Bus
        // reply would stay stuck at "Downloading" in the transfer UI forever.
        m_pendingProgress.remove(downloadId);
        m_pendingFinal.insert(downloadId, { status, transferStatus, reason });
        return;
    }

    m_transferClient->finishTransfer(transferId, transferStatus, reason);
    m_transfer2downloadMap.remove(transferId);
    m_download2transferMap.remove(downloadId);

    releaseDownload(downloadId);
    checkAllTransfers();
}

void DownloadManager::releaseDownload(int downloadId)
{
    if (WebKitDownload *download = m_downloadIdToObject.take(downloadId)) {
        m_downloadObjectToId.remove(download);
        g_object_unref(download);
    }
    m_downloadInfoCache.remove(downloadId);
    m_pendingProgress.remove(downloadId);
    m_pendingFinal.remove(downloadId);
}

void DownloadManager::checkAllTransfers()
{
    if (!existActiveTransfers()) {
        emit allTransfersCompleted();
    }
}
