/****************************************************************************
**
** Copyright (c) 2021 Open Mobile Platform LLC.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef FAVICONMANAGER_H
#define FAVICONMANAGER_H

#include <QObject>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QSize>
#include <QStringList>

class WPEWebPage;

class FaviconManager : public QObject
{
    Q_OBJECT

public:
    static FaviconManager *instance();
    static QString sanitizedHostname(const QString &hostname);

    Q_INVOKABLE void add(const QString &type, const QString &hostname, const QString &favicon, bool hasTouchIcon);
    Q_INVOKABLE void remove(const QString &type, const QString &hostname);

    Q_INVOKABLE QString get(const QString &type, const QString &hostname);

    Q_INVOKABLE void grabIcon(const QString &type, WPEWebPage *webPage, const QSize &size);
    Q_INVOKABLE void clear(const QString &type);

    static QString defaultDesktopBookmarkIcon();
    static bool isRealIcon(const QString &favicon);

signals:
    // Emitted whenever an icon is learned for a host, so the history and
    // bookmark models can refresh the rows already on screen — favicons arrive
    // asynchronously, long after the model handed out its data().
    void iconChanged(const QString &type, const QString &hostname, const QString &favicon);

private:
    FaviconManager(QObject *parent = nullptr);

    void save(const QString &type);
    void load(const QString &type);

    void fetchCandidate(const QString &type, const QString &pageUrl, const QStringList &candidates,
                        int index, QPointer<WPEWebPage> webPage, const QSize &size,
                        bool allowThumbnailFallback);
    void grabThumbnailFallback(const QString &type, QPointer<WPEWebPage> webPage, const QSize &size);

    struct Favicon {
        QString favicon;
        bool hasTouchIcon;
    };

    struct FaviconSet {
        bool loaded;
        QMap<QString, Favicon> favicons;
    };

    QMap<QString, FaviconSet> m_faviconSets;
    // Hosts whose stored entry is a page thumbnail and that have already been
    // re-tried for a real favicon in this session. Without this, every load of
    // an icon-less site would re-run the whole candidate chain.
    QSet<QString> m_thumbnailUpgradeAttempted;
};

#endif // FAVICONMANAGER_H
