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

#include <functional>

#include <QDateTime>
#include <QObject>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
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
    // What a download looked like, kept so that something other than the
    // transfer UI can ask about it — browser.downloads needs search(), which
    // the transfer engine cannot answer. Lives for the session only: the
    // download/transfer mappings are not persistent either (see the TODO
    // below), so a restart starts with an empty list.
    struct Record {
        int id = 0;
        QString url;
        QString path;      // absolute; empty until a destination is chosen
        QString mimeType;
        QString error;
        // "in_progress", "complete" or "interrupted", as the API spells them.
        QString state = QStringLiteral("in_progress");
        qint64 bytesReceived = 0;
        qint64 totalBytes = 0;
        QDateTime startTime;
        QDateTime endTime;
    };

    static DownloadManager *instance();

    bool existActiveTransfers();

    QList<Record> downloadRecords() const;
    Record downloadRecord(int downloadId) const;
    // Starts a download that no page asked for. `fileName` is a bare name (no
    // directories); when `saveAs` is false the destination is chosen without
    // prompting, which is what an API caller expects. Returns the download id,
    // or -1 with `error` set.
    int startDownload(const QString &url, const QString &fileName, bool saveAs, QString *error);
    // Forgets the record. The file, if any, is left alone.
    bool eraseDownloadRecord(int downloadId);
    bool removeDownloadFile(int downloadId, QString *error);

    bool pdfPrinting() const;

signals:
    void downloadStarted();
    void downloadStatusChanged(int downloadId, int status, QVariant info);
    void pdfPrintingChanged();
    void allTransfersCompleted();
    // Emitted when a download needs the user to choose a destination. The UI
    // replies with confirmDownload() (with the chosen absolute path) or
    // cancelPendingDownload().
    void saveAsRequested(int downloadId, QString suggestedFileName, QString defaultDir);
    // Record lifecycle, for consumers that need more than the transfer UI.
    void downloadRecordCreated(int downloadId);
    void downloadRecordChanged(int downloadId);
    void downloadRecordErased(int downloadId);

public slots:
    void cancelActiveTransfers();
    void cancel(int downloadId);

    // Called by the QML "Save As" prompt once the user has chosen (or declined).
    Q_INVOKABLE void confirmDownload(int downloadId, const QString &destinationPath);
    // Auto-save path for the "Save destination" setting: the user picked a
    // directory once, not this file name, so the name is sanitised here and
    // uniquified rather than silently overwriting an existing file.
    Q_INVOKABLE void confirmDownloadToDirectory(int downloadId, const QString &directory,
                                                const QString &fileName);
    Q_INVOKABLE void cancelPendingDownload(int downloadId);

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
    void releaseDownload(int downloadId);
    QVariantMap downloadInfo(int downloadId) const;
    void updateRecord(int downloadId, const std::function<void(Record &)> &mutate);

    // A download completion that arrived before the asynchronous createDownload
    // reply assigned a transfer id; flushed to the transfer engine once it does.
    struct PendingFinal {
        DownloadStatus::Status status;
        int transferStatus;
        QString reason;
    };

    // TODO: unlike Gecko downloads and Sailfish transfers these mappings
    //       are not persistent -> after user has browser closed transfers can't be
    //       restarted.
    QHash<qulonglong, int> m_download2transferMap;
    QHash<int, qulonglong> m_transfer2downloadMap;
    QHash<qulonglong, DownloadStatus::Status> m_statusCache;
    QHash<WebKitDownload*, int> m_downloadObjectToId;
    QHash<int, WebKitDownload*> m_downloadIdToObject;
    QHash<int, QVariantMap> m_downloadInfoCache;
    QHash<int, PendingFinal> m_pendingFinal;
    QHash<int, double> m_pendingProgress;
    QHash<int, Record> m_records;
    // Downloads started through startDownload(): the destination is settled
    // here instead of by asking the user.
    QHash<WebKitDownload *, QString> m_autoDestination;

    // Last progress state reported to the transfer engine, used to throttle
    // updates: sent unthrottled, the per-network-chunk received-data signal
    // floods the session bus (a 1 GB download queues ~65k calls, each a
    // synchronous SQLite write on the transfer-engine side), wedging D-Bus for
    // everything — progress display, the cancel callback, even openUrl.
    struct ProgressSent {
        qint64 elapsedMs;
        double progress;
    };
    QHash<int, ProgressSent> m_progressSent;
    QElapsedTimer m_progressClock;

    TransferEngineInterface *m_transferClient;

    bool m_pdfPrinting;
    int m_nextDownloadId = 1;

    friend class Browser;
};

#endif
