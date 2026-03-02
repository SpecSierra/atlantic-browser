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
#include <QDir>
#include <QFile>
#include <QDebug>

static DownloadManager *gSingleton = nullptr;

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
    Q_UNUSED(downloadId)
    // WPE: no-op; WebKit downloads handled separately
}

void DownloadManager::cancelTransfer(int transferId)
{
    m_transferClient->finishTransfer(transferId,
                                     TransferEngineData::TransferInterrupted,
                                     QString("Transfer got unavailable"));
}

void DownloadManager::restartTransfer(int transferId)
{
    m_transferClient->finishTransfer(transferId,
                                     TransferEngineData::TransferInterrupted,
                                     QString("Transfer got unavailable"));
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

void DownloadManager::checkAllTransfers()
{
    if (!existActiveTransfers()) {
        emit allTransfersCompleted();
    }
}
