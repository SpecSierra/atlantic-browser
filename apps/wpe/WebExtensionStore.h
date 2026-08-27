/*
 * Atlantic Browser — extension store.
 *
 * A curated local catalog (data/extension-catalog.json) rendered over add-on
 * metadata pulled live from addons.mozilla.org, plus a full AMO search so the
 * user is never limited to the catalog. Nothing is mirrored: packages are
 * downloaded straight from AMO, hash-checked against the digest AMO publishes,
 * and handed to WebExtensionManager::install().
 *
 * The catalog recommends; it never restricts. install() will fetch anything the
 * user asks for, including add-ons whose verdict is "broken" — the UI warns,
 * this class obeys.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <QAbstractListModel>
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

class WebExtensionStore : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY searchingChanged)

public:
    enum Roles {
        SlugRole = Qt::UserRole + 1,
        NameRole,
        SummaryRole,
        IconUrlRole,
        VersionRole,
        UsersRole,
        VerdictRole,        // "works" | "partial" | "broken" | "unknown"
        VerdictReasonRole,  // the APIs behind a partial/broken verdict
        NoteRole,           // hand-written, catalog entries only
        CuratedRole,
        VerifiedRole,       // somebody ran it on a device
        HomepageRole,
        InstalledRole,
        InstallingRole
    };

    static WebExtensionStore *instance();

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool busy() const { return m_pending > 0 || !m_installing.isEmpty(); }
    bool searching() const { return m_searching; }
    QString status() const { return m_status; }

    // Shows the curated catalog and refreshes each entry from AMO. Entries stay
    // visible with their shipped name and verdict when AMO is unreachable.
    Q_INVOKABLE void showCatalog();
    // Called when the store page opens. The catalog renders from the shipped
    // JSON with no icons or versions until something fetches AMO metadata, so
    // the first view has to trigger that itself; later visits are a no-op, and
    // it never clobbers search results the user is looking at.
    Q_INVOKABLE void ensureCatalogLoaded();
    // Full-text AMO search. An empty query returns to the catalog.
    Q_INVOKABLE void search(const QString &query);
    // Downloads and installs by AMO slug — no verdict gate, by design.
    Q_INVOKABLE void install(const QString &slug);
    // The "install it anyway" entry point: accepts a slug, an AMO add-on page
    // URL, or a direct .xpi URL. A direct URL cannot be hash-checked and says so.
    Q_INVOKABLE void installFromUserInput(const QString &text);
    Q_INVOKABLE void cancel();

    // Compatibility verdict for a manifest permission list, as AMO reports it
    // (host patterns and API names mixed together, MV2 style). `reasons` gets
    // the API names responsible. Shared by the catalog, search results and the
    // installed-extension warnings so one rule governs all three.
    static QString verdictFor(const QStringList &permissions, QStringList *reasons);

Q_SIGNALS:
    void countChanged();
    void busyChanged();
    void statusChanged();
    void searchingChanged();
    void installed(const QString &slug, const QString &name);
    void installFailed(const QString &slug, const QString &error);

private:
    explicit WebExtensionStore(QObject *parent = nullptr);

    struct Entry {
        QString slug;
        QString name;
        QString summary;
        QString iconUrl;
        QString version;
        QString homepage;
        QString note;
        QString verdict = QStringLiteral("unknown");
        QStringList verdictReasons;
        QString geckoId;
        int users = 0;
        bool curated = false;
        bool verified = false;
        bool installing = false;
    };

    void loadCatalog();
    void setStatus(const QString &status);
    void setPending(int pending);
    int indexOfSlug(const QString &slug) const;
    void applyAddonJson(Entry &entry, const QJsonObject &addon) const;
    Entry entryFromAddonJson(const QJsonObject &addon) const;
    bool isInstalled(const Entry &entry) const;

    QNetworkReply *get(const QString &url);
    void requestAddon(const QString &slug, bool forInstall);
    void startDownload(const QString &slug, const QString &url, const QString &expectedHash,
                       const QString &name);
    void finishDownload(const QString &slug, const QString &expectedHash, const QString &name,
                        QNetworkReply *reply);

    QList<Entry> m_catalog;   // shipped list, in catalog order
    QList<Entry> m_rows;      // what the model shows (catalog or search results)
    QNetworkAccessManager *m_network = nullptr;
    QList<QPointer<QNetworkReply>> m_inFlight;
    QString m_status;
    QString m_installing;     // slug currently being downloaded
    int m_pending = 0;
    bool m_searching = false;
    bool m_catalogRefreshed = false;
};
