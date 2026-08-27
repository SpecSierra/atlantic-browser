/*
 * Atlantic Browser — extension store.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "WebExtensionStore.h"

#include "WebExtension.h"
#include "WebExtensionManager.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>

namespace {

// AMO's public read API. v5 needs no key and no account.
const char kAmoAddon[] = "https://addons.mozilla.org/api/v5/addons/addon/%1/?lang=%2";
const char kAmoSearch[] = "https://addons.mozilla.org/api/v5/addons/search/";
const int kSearchPageSize = 30;

// APIs whose absence stops the extension doing its job at all.
const char *const kBrokenApis[] = {
    "webRequest", "webRequestBlocking", "declarativeNetRequest",
    "declarativeNetRequestWithHostAccess", "proxy", "dns", nullptr
};

// APIs whose absence costs a feature but usually not the point of the add-on.
const char *const kPartialApis[] = {
    // cookies, history, bookmarks and downloads are implemented against the
    // browser's own subsystems and are no longer listed here; what they do not
    // cover is documented in docs/investigations/webextensions.md.
    "webNavigation",
    "management", "idle", "browsingData", "sessions", "topSites", "search",
    "devtools", "nativeMessaging", "privacy", "tabHide",
    nullptr
};

bool listContains(const char *const *list, const QString &value)
{
    for (int i = 0; list[i]; ++i) {
        if (value == QLatin1String(list[i]))
            return true;
    }
    return false;
}

// AMO returns localized fields as { "en-US": "…" } even when `lang` is given,
// and as a plain string on some endpoints. Accept both.
QString localized(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (!value.isObject())
        return QString();

    const QJsonObject object = value.toObject();
    const QString exact = QLocale().name().replace(QLatin1Char('_'), QLatin1Char('-'));
    if (object.contains(exact))
        return object.value(exact).toString();

    const QString language = exact.section(QLatin1Char('-'), 0, 0);
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.key().section(QLatin1Char('-'), 0, 0) == language)
            return it.value().toString();
    }
    if (object.contains(QStringLiteral("en-US")))
        return object.value(QStringLiteral("en-US")).toString();
    return object.isEmpty() ? QString() : object.constBegin().value().toString();
}

QStringList toStringList(const QJsonValue &value)
{
    QStringList out;
    const QJsonArray array = value.toArray();
    for (const QJsonValue &entry : array) {
        if (entry.isString())
            out << entry.toString();
    }
    return out;
}

// current_version.file (v5) or current_version.files[0] (older responses).
QJsonObject fileObject(const QJsonObject &currentVersion)
{
    const QJsonValue single = currentVersion.value(QStringLiteral("file"));
    if (single.isObject())
        return single.toObject();
    const QJsonArray files = currentVersion.value(QStringLiteral("files")).toArray();
    return files.isEmpty() ? QJsonObject() : files.first().toObject();
}

} // namespace

WebExtensionStore *WebExtensionStore::instance()
{
    static WebExtensionStore *self = new WebExtensionStore;
    return self;
}

WebExtensionStore::WebExtensionStore(QObject *parent)
    : QAbstractListModel(parent)
{
    // An install anywhere (store, file picker, sideload) changes the Installed
    // badge on every row.
    connect(WebExtensionManager::instance(), &WebExtensionManager::extensionsChanged,
            this, [this]() {
                if (!m_rows.isEmpty()) {
                    Q_EMIT dataChanged(index(0), index(m_rows.size() - 1), { InstalledRole });
                }
            });
}

QString WebExtensionStore::verdictFor(const QStringList &permissions, QStringList *reasons)
{
    QStringList broken;
    QStringList partial;
    for (const QString &permission : permissions) {
        // AMO mixes host match patterns into the same list; they never affect
        // the verdict, since host access is exactly what we do support.
        if (permission.contains(QLatin1String("://"))
            || permission == QLatin1String("<all_urls>")) {
            continue;
        }
        if (listContains(kBrokenApis, permission))
            broken << permission;
        else if (listContains(kPartialApis, permission))
            partial << permission;
    }

    if (reasons)
        *reasons = broken + partial;
    if (!broken.isEmpty())
        return QStringLiteral("broken");
    if (!partial.isEmpty())
        return QStringLiteral("partial");
    return QStringLiteral("works");
}

// --- model ------------------------------------------------------------------

int WebExtensionStore::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QHash<int, QByteArray> WebExtensionStore::roleNames() const
{
    return {
        { SlugRole, "slug" },
        { NameRole, "name" },
        { SummaryRole, "summary" },
        { IconUrlRole, "iconUrl" },
        { VersionRole, "version" },
        { UsersRole, "users" },
        { VerdictRole, "verdict" },
        { VerdictReasonRole, "verdictReasons" },
        { HomepageRole, "homepage" },
        { InstalledRole, "installed" },
        { InstallingRole, "installing" }
    };
}

bool WebExtensionStore::isInstalled(const Entry &entry) const
{
    WebExtensionManager *manager = WebExtensionManager::instance();
    if (!entry.geckoId.isEmpty()
        && !manager->extensionInfo(WebExtension::idForGeckoId(entry.geckoId)).isEmpty()) {
        return true;
    }
    // Add-ons that declare no gecko id get an id derived from their manifest
    // name, which AMO's display name usually — but not always — matches. Only
    // the "installed" badge rides on this, so a miss is cosmetic.
    return !entry.name.isEmpty()
        && !manager->extensionInfo(WebExtension::idForName(entry.name)).isEmpty();
}

QVariant WebExtensionStore::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= m_rows.size())
        return QVariant();

    const Entry &entry = m_rows.at(index.row());
    switch (role) {
    case SlugRole: return entry.slug;
    case NameRole: return entry.name.isEmpty() ? entry.slug : entry.name;
    case SummaryRole: return entry.summary;
    case IconUrlRole: return entry.iconUrl;
    case VersionRole: return entry.version;
    case UsersRole: return entry.users;
    case VerdictRole: return entry.verdict;
    case VerdictReasonRole: return entry.verdictReasons;
    case HomepageRole: return entry.homepage;
    case InstalledRole: return isInstalled(entry);
    case InstallingRole: return entry.installing;
    default: return QVariant();
    }
}

int WebExtensionStore::indexOfSlug(const QString &slug) const
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).slug == slug)
            return i;
    }
    return -1;
}

void WebExtensionStore::setStatus(const QString &status)
{
    if (m_status == status)
        return;
    m_status = status;
    Q_EMIT statusChanged();
}

void WebExtensionStore::setPending(int pending)
{
    const bool wasBusy = busy();
    m_pending = qMax(0, pending);
    if (busy() != wasBusy)
        Q_EMIT busyChanged();
}

// --- AMO --------------------------------------------------------------------

QNetworkReply *WebExtensionStore::get(const QString &url)
{
    if (!m_network)
        m_network = new QNetworkAccessManager(this);

    QNetworkRequest request{ QUrl(url) };
    // AMO 403s an empty UA, and the download URL redirects to a CDN.
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Atlantic/%1 (SailfishOS)")
                          .arg(QCoreApplication::applicationVersion()));
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    QNetworkReply *reply = m_network->get(request);
    m_inFlight.append(reply);
    return reply;
}

void WebExtensionStore::applyAddonJson(Entry &entry, const QJsonObject &addon) const
{
    const QString name = localized(addon.value(QStringLiteral("name")));
    if (!name.isEmpty())
        entry.name = name;
    const QString summary = localized(addon.value(QStringLiteral("summary")));
    if (!summary.isEmpty())
        entry.summary = summary;

    entry.slug = addon.value(QStringLiteral("slug")).toString(entry.slug);
    entry.geckoId = addon.value(QStringLiteral("guid")).toString();
    entry.iconUrl = addon.value(QStringLiteral("icon_url")).toString();
    entry.users = addon.value(QStringLiteral("average_daily_users")).toInt();
    entry.homepage = localized(addon.value(QStringLiteral("homepage")).toObject()
                                   .value(QStringLiteral("url")));
    if (entry.homepage.isEmpty())
        entry.homepage = addon.value(QStringLiteral("url")).toString();

    const QJsonObject currentVersion = addon.value(QStringLiteral("current_version")).toObject();
    entry.version = currentVersion.value(QStringLiteral("version")).toString();

    const QJsonObject file = fileObject(currentVersion);
    QStringList permissions = toStringList(file.value(QStringLiteral("permissions")));
    permissions += toStringList(file.value(QStringLiteral("host_permissions")));

    QStringList reasons;
    entry.verdict = verdictFor(permissions, &reasons);
    entry.verdictReasons = reasons;
}

WebExtensionStore::Entry WebExtensionStore::entryFromAddonJson(const QJsonObject &addon) const
{
    Entry entry;
    applyAddonJson(entry, addon);
    return entry;
}

void WebExtensionStore::search(const QString &query)
{
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        // Nothing to fall back to: the store has no list of its own, so an
        // empty query empties the view.
        cancel();
        beginResetModel();
        m_rows.clear();
        endResetModel();
        Q_EMIT countChanged();
        if (m_searching) {
            m_searching = false;
            Q_EMIT searchingChanged();
        }
        setStatus(QString());
        return;
    }

    cancel();
    if (!m_searching) {
        m_searching = true;
        Q_EMIT searchingChanged();
    }
    setStatus(tr("Searching…"));

    QUrl url{ QString::fromLatin1(kAmoSearch) };
    QUrlQuery parameters;
    parameters.addQueryItem(QStringLiteral("q"), trimmed);
    parameters.addQueryItem(QStringLiteral("app"), QStringLiteral("firefox"));
    parameters.addQueryItem(QStringLiteral("type"), QStringLiteral("extension"));
    parameters.addQueryItem(QStringLiteral("page_size"), QString::number(kSearchPageSize));
    parameters.addQueryItem(QStringLiteral("lang"),
                            QLocale().name().replace(QLatin1Char('_'), QLatin1Char('-')));
    url.setQuery(parameters);

    setPending(m_pending + 1);
    QNetworkReply *reply = get(url.toString());
    connect(reply, &QNetworkReply::finished, this, [this, reply, trimmed]() {
        reply->deleteLater();
        m_inFlight.removeAll(reply);
        setPending(m_pending - 1);

        if (reply->error() != QNetworkReply::NoError) {
            setStatus(tr("Search failed: %1").arg(reply->errorString()));
            return;
        }

        const QJsonArray results =
            QJsonDocument::fromJson(reply->readAll()).object()
                .value(QStringLiteral("results")).toArray();

        beginResetModel();
        m_rows.clear();
        for (const QJsonValue &value : results)
            m_rows.append(entryFromAddonJson(value.toObject()));
        endResetModel();
        Q_EMIT countChanged();

        setStatus(m_rows.isEmpty() ? tr("Nothing found for “%1”.").arg(trimmed) : QString());
    });
}

void WebExtensionStore::cancel()
{
    for (const QPointer<QNetworkReply> &reply : m_inFlight) {
        if (reply)
            reply->abort();
    }
    m_inFlight.clear();
    setPending(0);
}

// --- install ----------------------------------------------------------------

void WebExtensionStore::install(const QString &slug)
{
    if (!m_installing.isEmpty()) {
        setStatus(tr("Already installing %1.").arg(m_installing));
        return;
    }
    requestAddon(slug, true);
}

void WebExtensionStore::installFromUserInput(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return;

    // A direct package URL: no AMO metadata, so no publisher hash to check
    // against. Allowed — this is the deliberate escape hatch — but said out loud.
    if (trimmed.endsWith(QLatin1String(".xpi"), Qt::CaseInsensitive)
        || trimmed.endsWith(QLatin1String(".zip"), Qt::CaseInsensitive)) {
        setStatus(tr("Downloading an unverified package."));
        startDownload(trimmed, trimmed, QString(), QUrl(trimmed).fileName());
        return;
    }

    // An AMO add-on page URL: .../firefox/addon/<slug>/
    QString slug = trimmed;
    if (trimmed.contains(QLatin1String("://"))) {
        const QStringList segments = QUrl(trimmed).path().split(QLatin1Char('/'),
                                                                QString::SkipEmptyParts);
        const int marker = segments.indexOf(QStringLiteral("addon"));
        if (marker >= 0 && marker + 1 < segments.size())
            slug = segments.at(marker + 1);
        else if (!segments.isEmpty())
            slug = segments.last();
    }
    install(slug);
}

void WebExtensionStore::requestAddon(const QString &slug, bool forInstall)
{
    setPending(m_pending + 1);
    QNetworkReply *reply = get(QString::fromLatin1(kAmoAddon)
                                   .arg(slug, QLocale().name().replace(QLatin1Char('_'),
                                                                       QLatin1Char('-'))));
    connect(reply, &QNetworkReply::finished, this, [this, reply, slug, forInstall]() {
        reply->deleteLater();
        m_inFlight.removeAll(reply);
        setPending(m_pending - 1);

        if (reply->error() != QNetworkReply::NoError) {
            const QString error = reply->error() == QNetworkReply::ContentNotFoundError
                ? tr("No add-on called “%1” on addons.mozilla.org.").arg(slug)
                : reply->errorString();
            setStatus(error);
            if (forInstall)
                Q_EMIT installFailed(slug, error);
            return;
        }

        const QJsonObject addon = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonObject file =
            fileObject(addon.value(QStringLiteral("current_version")).toObject());
        const QString url = file.value(QStringLiteral("url")).toString();
        const QString name = localized(addon.value(QStringLiteral("name")));

        if (!forInstall)
            return;
        if (url.isEmpty()) {
            const QString error = tr("%1 has no downloadable package.").arg(name.isEmpty() ? slug : name);
            setStatus(error);
            Q_EMIT installFailed(slug, error);
            return;
        }
        startDownload(slug, url, file.value(QStringLiteral("hash")).toString(), name);
    });
}

void WebExtensionStore::startDownload(const QString &slug, const QString &url,
                                      const QString &expectedHash, const QString &name)
{
    m_installing = slug;
    const int row = indexOfSlug(slug);
    if (row >= 0) {
        m_rows[row].installing = true;
        Q_EMIT dataChanged(index(row), index(row), { InstallingRole });
    }
    Q_EMIT busyChanged();
    setStatus(tr("Downloading %1…").arg(name.isEmpty() ? slug : name));

    QNetworkReply *reply = get(url);
    connect(reply, &QNetworkReply::finished, this, [this, reply, slug, expectedHash, name]() {
        reply->deleteLater();
        m_inFlight.removeAll(reply);
        finishDownload(slug, expectedHash, name, reply);
    });
}

void WebExtensionStore::finishDownload(const QString &slug, const QString &expectedHash,
                                       const QString &name, QNetworkReply *reply)
{
    const QString label = name.isEmpty() ? slug : name;

    const auto fail = [&](const QString &error) {
        m_installing.clear();
        const int row = indexOfSlug(slug);
        if (row >= 0) {
            m_rows[row].installing = false;
            Q_EMIT dataChanged(index(row), index(row), { InstallingRole });
        }
        Q_EMIT busyChanged();
        setStatus(error);
        Q_EMIT installFailed(slug, error);
    };

    if (reply->error() != QNetworkReply::NoError) {
        fail(tr("Could not download %1: %2").arg(label, reply->errorString()));
        return;
    }

    const QByteArray payload = reply->readAll();
    if (payload.isEmpty()) {
        fail(tr("Could not download %1: the package was empty.").arg(label));
        return;
    }

    // AMO publishes the digest as "sha256:<hex>". A mismatch means the bytes are
    // not what AMO signed off on; refuse rather than install them.
    if (!expectedHash.isEmpty()) {
        const QString algorithm = expectedHash.section(QLatin1Char(':'), 0, 0).toLower();
        const QString digest = expectedHash.section(QLatin1Char(':'), 1);
        if (algorithm == QLatin1String("sha256")) {
            const QString actual = QString::fromLatin1(
                QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
            if (actual.compare(digest, Qt::CaseInsensitive) != 0) {
                fail(tr("%1 failed its checksum and was not installed.").arg(label));
                return;
            }
        } else {
            qWarning() << "[WEBEXT-STORE] unknown digest algorithm" << algorithm
                       << "- installing without verification";
        }
    }

    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/extension-downloads");
    QDir().mkpath(directory);
    const QString path = QStringLiteral("%1/%2.xpi").arg(directory, slug);

    QFile package(path);
    if (!package.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(tr("Could not save %1: %2").arg(label, package.errorString()));
        return;
    }
    package.write(payload);
    package.close();

    const bool ok = WebExtensionManager::instance()->install(path);
    QFile::remove(path);

    m_installing.clear();
    const int row = indexOfSlug(slug);
    if (row >= 0) {
        m_rows[row].installing = false;
        Q_EMIT dataChanged(index(row), index(row), { InstallingRole, InstalledRole });
    }
    Q_EMIT busyChanged();

    if (ok) {
        setStatus(tr("Installed %1.").arg(label));
        Q_EMIT installed(slug, label);
    } else {
        const QString error = WebExtensionManager::instance()->lastError();
        setStatus(error.isEmpty() ? tr("Could not install %1.").arg(label) : error);
        Q_EMIT installFailed(slug, error);
    }
}
