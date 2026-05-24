/****************************************************************************
**
** Copyright (c) 2013 Jolla Ltd.
** Contact: Dmitry Rozhkov <dmitry.rozhkov@jollamobile.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOWNLOADMANAGER_H
#define DOWNLOADMANAGER_H

#include "downloadstatus.h"

#include <QObject>
#include <QHash>
#include <QString>
#include <QVariant>

class TransferEngineInterface;
typedef struct _WebKitDownload WebKitDownload;

class DownloadManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool pdfPrinting READ pdfPrinting NOTIFY pdfPrintingChanged FINAL)
    Q_ENUMS(DownloadStatus::Status)

public:
    static DownloadManager *instance();

    bool existActiveTransfers();

    bool pdfPrinting() const;

signals:
    void downloadStarted();
    void downloadStatusChanged(int downloadId, int status, QVariant info);
    void pdfPrintingChanged();
    void allTransfersCompleted();

public slots:
    void cancelActiveTransfers();
    void cancel(int downloadId);

public:
    bool prepareDownload(WebKitDownload *download, const QString &suggestedFilename);
    void updateDownload(WebKitDownload *download);
    void downloadFinished(WebKitDownload *download);
    void downloadFailed(WebKitDownload *download, const QString &reason);

private slots:

private:
    explicit DownloadManager();
    ~DownloadManager();

    void checkAllTransfers();

    void cancelTransfer(int transferId);
    void restartTransfer(int transferId);

    void setPdfPrinting(const bool pdfPrinting);
    int ensureDownloadId(WebKitDownload *download);
    QString ensureDestinationPath(const QString &suggestedFilename) const;
    int transferIdForDownload(int downloadId) const;
    void finalizeDownload(int downloadId, DownloadStatus::Status status, int transferStatus, const QString &reason);
    QVariantMap downloadInfo(int downloadId) const;

    // TODO: unlike Gecko downloads and Sailfish transfers these mappings
    //       are not persistent -> after user has browser closed transfers can't be
    //       restarted.
    QHash<qulonglong, int> m_download2transferMap;
    QHash<int, qulonglong> m_transfer2downloadMap;
    QHash<qulonglong, DownloadStatus::Status> m_statusCache;
    QHash<WebKitDownload*, int> m_downloadObjectToId;
    QHash<int, WebKitDownload*> m_downloadIdToObject;
    QHash<int, QVariantMap> m_downloadInfoCache;

    TransferEngineInterface *m_transferClient;

    bool m_pdfPrinting;
    int m_nextDownloadId = 1;

    friend class Browser;
};

#endif
