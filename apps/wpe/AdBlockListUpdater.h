/*
 * Copyright (c) 2026
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

// Weekly refresh of the adblock filter engine. CI republishes engine.dat +
// adblock-resources.json (+ engine.version epoch stamp) to GitHub Pages on a
// cron; this downloads them into the jail-whitelisted app cache when the
// remote stamp is newer than the best local copy. The fresh engine is picked
// up on the next browser start (both UI process and WebProcess extension load
// whichever of shipped/cached has the higher engine.version).
class AdBlockListUpdater : public QObject {
    Q_OBJECT
public:
    // Schedule a background check shortly after startup (no-op within 7 days
    // of the last one, or with ATLANTIC_ADBLOCK_UPDATE=0).
    static void start();

    // Directory the updater downloads into (…/org.sailfishos/browser/adblock).
    static QString cacheDir();
    // engine.version stamp in dir, 0 if absent/invalid.
    static qlonglong versionIn(const QString& dir);

private:
    explicit AdBlockListUpdater(QObject* parent = nullptr);

    void checkNow();
    void onVersionReply(QNetworkReply* reply);
    void fetchNextFile();
    void onFileReply(QNetworkReply* reply);

    QNetworkAccessManager* m_nam = nullptr;
    QString m_baseUrl;
    qlonglong m_remoteVersion = 0;
    QStringList m_pendingFiles;
    QString m_currentFile;
};
