/*
 * Atlantic Browser — WebExtension host.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "WebExtensionManager.h"

#include "WebExtensionArchive.h"
#include "WebExtensionBackground.h"
#include "WebExtensionBackgroundView.h"
#include "WebExtensionScripts.h"
#include "WPEWebPage.h"

#include <QDateTime>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantMap>

#include <memory>
#include <vector>

// Qt's `#define signals public` mangles a GDBus struct field of the same name;
// the GLib headers WebKit pulls in have to be seen with the keyword undefined.
#pragma push_macro("signals")
#undef signals
#include <jsc/jsc.h>
#include <libsoup/soup.h>
#pragma pop_macro("signals")


namespace {

const char *const kRegistryFile = "registry.json";

QString jsonLiteral(const QJsonValue &value)
{
    QByteArray json = QJsonDocument(QJsonArray{ value }).toJson(QJsonDocument::Compact).trimmed();
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

QString compact(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

// GObject signal details and script world names have to be plain identifiers;
// extension ids may contain '.' and '-'.
QString sanitize(const QString &id)
{
    QString out = id;
    for (int i = 0; i < out.size(); ++i) {
        const QChar c = out.at(i);
        if (!c.isLetterOrNumber() && c != QLatin1Char('_'))
            out[i] = QLatin1Char('_');
    }
    return out;
}

bool removeTree(const QString &path)
{
    QDir dir(path);
    return dir.exists() ? dir.removeRecursively() : true;
}

bool copyTree(const QString &from, const QString &to)
{
    QDir source(from);
    if (!source.exists())
        return false;
    if (!QDir().mkpath(to))
        return false;

    const QFileInfoList entries = source.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo &info : entries) {
        const QString target = QDir(to).filePath(info.fileName());
        if (info.isDir()) {
            if (!copyTree(info.absoluteFilePath(), target))
                return false;
        } else {
            QFile::remove(target);
            if (!QFile::copy(info.absoluteFilePath(), target))
                return false;
        }
    }
    return true;
}

QString mimeTypeForPath(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    static const QHash<QString, QString> kTypes {
        { QStringLiteral("html"), QStringLiteral("text/html") },
        { QStringLiteral("htm"), QStringLiteral("text/html") },
        { QStringLiteral("js"), QStringLiteral("text/javascript") },
        { QStringLiteral("mjs"), QStringLiteral("text/javascript") },
        { QStringLiteral("json"), QStringLiteral("application/json") },
        { QStringLiteral("css"), QStringLiteral("text/css") },
        { QStringLiteral("png"), QStringLiteral("image/png") },
        { QStringLiteral("jpg"), QStringLiteral("image/jpeg") },
        { QStringLiteral("jpeg"), QStringLiteral("image/jpeg") },
        { QStringLiteral("gif"), QStringLiteral("image/gif") },
        { QStringLiteral("svg"), QStringLiteral("image/svg+xml") },
        { QStringLiteral("webp"), QStringLiteral("image/webp") },
        { QStringLiteral("ico"), QStringLiteral("image/x-icon") },
        { QStringLiteral("woff"), QStringLiteral("font/woff") },
        { QStringLiteral("woff2"), QStringLiteral("font/woff2") },
        { QStringLiteral("ttf"), QStringLiteral("font/ttf") },
        { QStringLiteral("txt"), QStringLiteral("text/plain") },
        { QStringLiteral("xml"), QStringLiteral("text/xml") }
    };
    return kTypes.value(suffix, QStringLiteral("application/octet-stream"));
}

// WebKit's UserContentURLPattern does not know <all_urls>; everything else in
// the match-pattern syntax it shares with Chrome.
QStringList expandPatterns(const QStringList &patterns)
{
    QStringList out;
    for (const QString &pattern : patterns) {
        if (pattern == QLatin1String("<all_urls>")) {
            // Every scheme <all_urls> covers that WebKit will match on. file://
            // was missing, which quietly excluded local pages from every
            // content script declaring <all_urls> — found while testing an
            // extension against a file:// page and getting no injection at all.
            out << QStringLiteral("http://*/*") << QStringLiteral("https://*/*")
                << QStringLiteral("file:///*") << QStringLiteral("ftp://*/*");
        } else {
            out << pattern;
        }
    }
    out.removeDuplicates();
    return out;
}

// Builds the NULL-terminated char* vector WebKit wants, keeping the UTF-8
// buffers alive in `storage` for the duration of the call.
std::vector<const char *> patternVector(const QStringList &patterns,
                                        QVector<QByteArray> &storage)
{
    std::vector<const char *> out;
    for (const QString &pattern : patterns) {
        storage.append(pattern.toUtf8());
        out.push_back(storage.last().constData());
    }
    out.push_back(nullptr);
    return out;
}

} // namespace

WebExtensionManager *WebExtensionManager::instance()
{
    static WebExtensionManager *self = new WebExtensionManager;
    return self;
}

WebExtensionManager::WebExtensionManager(QObject *parent)
    : QAbstractListModel(parent)
{
}

WebExtensionManager::~WebExtensionManager() = default;

// --- paths ------------------------------------------------------------------

QString WebExtensionManager::extensionsDirectory() const
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QStringLiteral("/home/defaultuser/.local/share/org.sailfishos/browser");
    return base + QStringLiteral("/extensions");
}

QString WebExtensionManager::extensionDataDirectory()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QStringLiteral("/home/defaultuser/.local/share/org.sailfishos/browser");
    return base + QStringLiteral("/extension-data");
}

// --- model ------------------------------------------------------------------

int WebExtensionManager::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_entries.size();
}

QHash<int, QByteArray> WebExtensionManager::roleNames() const
{
    return {
        { ExtensionIdRole, "extensionId" },
        { NameRole, "name" },
        { VersionRole, "version" },
        { DescriptionRole, "description" },
        { EnabledRole, "enabled" },
        { IconPathRole, "iconPath" },
        { PermissionsRole, "permissions" },
        { HostPermissionsRole, "hostPermissions" },
        { PopupUrlRole, "popupUrl" },
        { OptionsUrlRole, "optionsUrl" },
        { HomepageRole, "homepage" },
        { HasBackgroundRole, "hasBackground" },
        { WarningsRole, "warnings" }
    };
}

QVariant WebExtensionManager::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= m_entries.size())
        return QVariant();

    const Entry &entry = m_entries.at(index.row());
    const WebExtension &extension = entry.extension;
    const QString base = QStringLiteral("%1://%2/").arg(QLatin1String(kScheme), extension.id());

    switch (role) {
    case ExtensionIdRole: return extension.id();
    case NameRole: return extension.name();
    case VersionRole: return extension.version();
    case DescriptionRole: return extension.description();
    case EnabledRole: return entry.enabled;
    case IconPathRole: {
        const QString icon = extension.bestIconPath();
        return icon.isEmpty() ? QString()
                              : QStringLiteral("file://") + QDir(extension.baseDir()).filePath(icon);
    }
    case PermissionsRole: return extension.permissions();
    case HostPermissionsRole: return extension.hostPermissions();
    case PopupUrlRole: {
        const QString popup = entry.popupOverride.isEmpty() ? extension.actionPopup()
                                                            : entry.popupOverride;
        return popup.isEmpty() ? QString() : base + popup;
    }
    case OptionsUrlRole:
        return extension.optionsPage().isEmpty() ? QString() : base + extension.optionsPage();
    case HomepageRole: return extension.homepageUrl();
    case HasBackgroundRole: return extension.hasBackground();
    case WarningsRole: return entry.warnings;
    default: return QVariant();
    }
}

int WebExtensionManager::indexOf(const QString &extensionId) const
{
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).extension.id() == extensionId)
            return i;
    }
    return -1;
}

WebExtensionManager::Entry *WebExtensionManager::entryFor(const QString &extensionId)
{
    const int index = indexOf(extensionId);
    return index < 0 ? nullptr : &m_entries[index];
}

const WebExtensionManager::Entry *WebExtensionManager::entryFor(const QString &extensionId) const
{
    const int index = indexOf(extensionId);
    return index < 0 ? nullptr : &m_entries.at(index);
}

void WebExtensionManager::setLastError(const QString &error)
{
    if (m_lastError == error)
        return;
    m_lastError = error;
    if (!error.isEmpty())
        qWarning() << "[WEBEXT]" << error;
    Q_EMIT lastErrorChanged();
}

// --- registry ---------------------------------------------------------------

void WebExtensionManager::loadRegistry()
{
    QFile file(QDir(extensionsDirectory()).filePath(QLatin1String(kRegistryFile)));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject registry = QJsonDocument::fromJson(file.readAll()).object();
    for (Entry &entry : m_entries) {
        if (!registry.contains(entry.extension.id())) {
            entry.installReason = QStringLiteral("install");
            continue;
        }
        const QJsonObject state = registry.value(entry.extension.id()).toObject();
        if (state.contains(QStringLiteral("enabled")))
            entry.enabled = state.value(QStringLiteral("enabled")).toBool(true);

        const QString stored = state.value(QStringLiteral("version")).toString();
        if (stored != entry.extension.version()) {
            entry.installReason = QStringLiteral("update");
            entry.previousVersion = stored;
        }
    }
}

void WebExtensionManager::saveRegistry() const
{
    QJsonObject registry;
    for (const Entry &entry : m_entries) {
        registry.insert(entry.extension.id(),
                        QJsonObject{ { QStringLiteral("enabled"), entry.enabled },
                                     { QStringLiteral("version"), entry.extension.version() } });
    }
    QDir().mkpath(extensionsDirectory());
    QFile file(QDir(extensionsDirectory()).filePath(QLatin1String(kRegistryFile)));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "[WEBEXT] cannot write the extension registry:" << file.errorString();
        return;
    }
    file.write(QJsonDocument(registry).toJson(QJsonDocument::Indented));
}

void WebExtensionManager::reload()
{
    beginResetModel();

    for (Entry &entry : m_entries)
        stopBackground(entry);
    m_entries.clear();

    const QString root = extensionsDirectory();
    QDir().mkpath(root);
    const QStringList directories = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : directories) {
        Entry entry;
        QString error;
        if (!entry.extension.loadFromDirectory(QDir(root).filePath(name), &error)) {
            qWarning() << "[WEBEXT] skipping" << name << "-" << error;
            continue;
        }

        // Surface what we knowingly will not run, rather than letting the
        // extension fail mysteriously at runtime.
        const QStringList permissions = entry.extension.permissions();
        if (permissions.contains(QStringLiteral("webRequest"))
            || permissions.contains(QStringLiteral("webRequestBlocking"))) {
            entry.warnings << QStringLiteral(
                "Uses webRequest, which Atlantic does not provide: request blocking and "
                "rewriting from this extension will not take effect.");
        }
        if (permissions.contains(QStringLiteral("declarativeNetRequest"))) {
            entry.warnings << QStringLiteral(
                "Uses declarativeNetRequest, which Atlantic does not provide.");
        }
        if (entry.extension.backgroundIsServiceWorker()) {
            entry.warnings << QStringLiteral(
                "Declares an MV3 service worker; it runs as a plain background script, "
                "with no service-worker lifecycle or events.");
        }

        m_entries.append(entry);
    }

    loadRegistry();
    for (Entry &entry : m_entries) {
        if (entry.enabled)
            startBackground(entry);
    }

    announceLifecycleEvents();

    endResetModel();
    Q_EMIT countChanged();
    Q_EMIT extensionsChanged();
    qDebug() << "[WEBEXT] loaded" << m_entries.size() << "extension(s) from" << root;
}

// --- install / uninstall ----------------------------------------------------

bool WebExtensionManager::install(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists()) {
        setLastError(QStringLiteral("No such file: %1").arg(path));
        return false;
    }

    QTemporaryDir staging;
    QString sourceDir;

    if (info.isDir()) {
        sourceDir = info.absoluteFilePath();
    } else {
        if (!staging.isValid()) {
            setLastError(QStringLiteral("Cannot create a staging directory"));
            return false;
        }
        QString archiveError;
        if (!WebExtensionArchive::extract(info.absoluteFilePath(), staging.path(),
                                          &archiveError)) {
            setLastError(QStringLiteral("Could not unpack %1: %2")
                             .arg(info.fileName(), archiveError));
            return false;
        }
        sourceDir = staging.path();
        // Archives from source forges wrap everything in a single top-level
        // directory; step into it when the manifest is not at the root.
        if (!QFile::exists(QDir(sourceDir).filePath(QStringLiteral("manifest.json")))) {
            const QStringList nested = QDir(sourceDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            if (nested.size() == 1)
                sourceDir = QDir(sourceDir).filePath(nested.first());
        }
    }

    WebExtension probe;
    QString error;
    if (!probe.loadFromDirectory(sourceDir, &error)) {
        setLastError(error);
        return false;
    }

    const QString target = QDir(extensionsDirectory()).filePath(probe.id());
    if (QFileInfo::exists(target) && !removeTree(target)) {
        setLastError(QStringLiteral("Cannot replace the existing copy of %1").arg(probe.name()));
        return false;
    }
    if (!copyTree(sourceDir, target)) {
        removeTree(target);
        setLastError(QStringLiteral("Cannot copy %1 into %2").arg(probe.name(), target));
        return false;
    }

    setLastError(QString());
    reload();
    return true;
}

bool WebExtensionManager::uninstall(const QString &extensionId)
{
    const int index = indexOf(extensionId);
    if (index < 0) {
        setLastError(QStringLiteral("No such extension: %1").arg(extensionId));
        return false;
    }

    stopBackground(m_entries[index]);
    const QString directory = m_entries.at(index).extension.baseDir();

    beginRemoveRows(QModelIndex(), index, index);
    m_entries.removeAt(index);
    endRemoveRows();

    if (!removeTree(directory)) {
        setLastError(QStringLiteral("Removed %1 from the list, but its files are still on disk")
                         .arg(extensionId));
    }
    removeTree(QDir(extensionDataDirectory()).filePath(extensionId));
    for (auto it = m_storageCache.begin(); it != m_storageCache.end();) {
        if (it.key().startsWith(extensionId + QLatin1Char('/')))
            it = m_storageCache.erase(it);
        else
            ++it;
    }

    saveRegistry();
    Q_EMIT countChanged();
    Q_EMIT extensionsChanged();
    return true;
}

void WebExtensionManager::setExtensionEnabled(const QString &extensionId, bool enabled)
{
    const int index = indexOf(extensionId);
    if (index < 0)
        return;

    Entry &entry = m_entries[index];
    if (entry.enabled == enabled)
        return;
    entry.enabled = enabled;

    if (enabled) {
        startBackground(entry);
    } else {
        stopBackground(entry);
        // A disabled extension must not keep offering menu items.
        entry.menuItems.clear();
        entry.dynamicScripts.clear();
        Q_EMIT contextMenuItemsChanged();
    }

    saveRegistry();
    const QModelIndex modelIndex = createIndex(index, 0);
    Q_EMIT dataChanged(modelIndex, modelIndex, { EnabledRole });
    Q_EMIT extensionsChanged();
}

QVariantMap WebExtensionManager::extensionInfo(const QString &extensionId) const
{
    QVariantMap info;
    const int index = indexOf(extensionId);
    if (index < 0)
        return info;
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it)
        info.insert(QString::fromUtf8(it.value()), data(createIndex(index, 0), it.key()));
    return info;
}

void WebExtensionManager::openActionPopup(const QString &extensionId)
{
    const Entry *entry = entryFor(extensionId);
    if (!entry)
        return;
    QString popup = entry->popupOverride.isEmpty() ? entry->extension.actionPopup()
                                                   : entry->popupOverride;
    if (popup.isEmpty()) {
        // No popup declared: a click is the action itself.
        emitEvent(extensionId, ExtContext(), QStringLiteral("action.onClicked"),
                  QJsonArray{ QJsonObject{ { QStringLiteral("id"),
                                             m_host ? m_host->extActiveTabId() : 0 } } });
        return;
    }
    Q_EMIT openUrlRequested(QStringLiteral("%1://%2/%3")
                                .arg(QLatin1String(kScheme), extensionId, popup),
                            true);
}

void WebExtensionManager::openUrl(const QString &url, bool inNewTab)
{
    if (!url.isEmpty())
        Q_EMIT openUrlRequested(url, inNewTab);
}

void WebExtensionManager::openOptionsPage(const QString &extensionId)
{
    const Entry *entry = entryFor(extensionId);
    if (!entry || entry->extension.optionsPage().isEmpty())
        return;
    Q_EMIT openUrlRequested(QStringLiteral("%1://%2/%3")
                                .arg(QLatin1String(kScheme), extensionId,
                                     entry->extension.optionsPage()),
                            true);
}

// --- background contexts ----------------------------------------------------

void WebExtensionManager::announceLifecycleEvents()
{
    // Only meaningful once a background context exists — before the engine is
    // ready there is nothing listening, and an onInstalled fired into nothing
    // is worse than a late one: the registry would record it as delivered.
    bool registryNeedsWriting = false;
    for (Entry &entry : m_entries) {
        if (!entry.enabled || (!entry.background && !entry.backgroundView))
            continue;
        if (!entry.installReason.isEmpty()) {
            QJsonObject details{ { QStringLiteral("reason"), entry.installReason } };
            if (!entry.previousVersion.isEmpty())
                details.insert(QStringLiteral("previousVersion"), entry.previousVersion);
            emitEvent(entry.extension.id(), ExtContext(),
                      QStringLiteral("runtime.onInstalled"), QJsonArray{ details });
            registryNeedsWriting = true;
        }
        emitEvent(entry.extension.id(), ExtContext(),
                  QStringLiteral("runtime.onStartup"), QJsonArray());
        entry.installReason.clear();
        entry.previousVersion.clear();
    }
    if (registryNeedsWriting)
        saveRegistry();
}

void WebExtensionManager::setEngineReady()
{
    if (m_engineReady)
        return;
    m_engineReady = true;

    // Spawning a WebProcess per background page competes with the first page
    // load, so let the browser settle first. Extensions are not so urgent that
    // they should slow the thing the user is actually waiting for.
    QTimer::singleShot(1500, this, [this]() {
        for (Entry &entry : m_entries) {
            if (entry.enabled && entry.extension.hasBackground()
                && !entry.background && !entry.backgroundView) {
                startBackground(entry);
            }
        }
        // The page needs a moment to load before its listeners exist.
        QTimer::singleShot(1500, this, [this]() { announceLifecycleEvents(); });
    });
}

void WebExtensionManager::startBackground(Entry &entry)
{
    stopBackground(entry);
    if (!entry.extension.hasBackground())
        return;

    // Deferred until a real view exists; setEngineReady() comes back for these.
    if (!m_engineReady)
        return;

    // A real page first: extensions written against Firefox event pages expect
    // a document, and the JSC host cannot give them one. Only if the offscreen
    // backend cannot be created do we fall back to it, which still serves
    // service-worker-shaped extensions.
    //
    // ATLANTIC_EXT_BACKGROUND_PAGE=0 forces the fallback. This path creates a
    // wpe_view_backend, and getting that wrong once already cost a startup
    // crash — an escape hatch that needs no rebuild is worth the two lines.
    static const bool backgroundPagesEnabled =
        qgetenv("ATLANTIC_EXT_BACKGROUND_PAGE").trimmed() != QByteArray("0");
    if (!backgroundPagesEnabled) {
        qDebug() << "[WEBEXT] background pages disabled by environment; using the JSC host";
    } else {
    const QString pageShim = buildShim(entry, QStringLiteral("page"),
                                       QStringLiteral("atlExtBg_") + sanitize(entry.extension.id()));
    auto *view = new WebExtensionBackgroundView(this, entry.extension, pageShim, this);
    if (view->start()) {
        entry.backgroundView = view;
        return;
    }
    delete view;
    qWarning() << "[WEBEXT]" << entry.extension.id()
               << "no offscreen background page; falling back to the JSC host";
    }

    const QString shim = buildShim(entry, QStringLiteral("background"), QString());
    auto *background = new WebExtensionBackground(this, entry.extension, shim, this);
    if (!background->start()) {
        delete background;
        return;
    }
    entry.background = background;
}

void WebExtensionManager::stopBackground(Entry &entry)
{
    if (entry.backgroundView) {
        entry.backgroundView->deleteLater();
        entry.backgroundView = nullptr;
    }
    if (entry.background) {
        entry.background->deleteLater();
        entry.background = nullptr;
    }
}

// --- URI scheme -------------------------------------------------------------

void WebExtensionManager::registerUriScheme()
{
    if (m_schemeRegistered)
        return;
    m_schemeRegistered = true;

    // Created here rather than in reload(): the caller adds this directory to
    // the WebProcess sandbox immediately afterwards, and addSandboxPathIfExists
    // skips paths that are not there yet.
    QDir().mkpath(extensionsDirectory());

    WebKitWebContext *context = webkit_web_context_get_default();
    webkit_web_context_register_uri_scheme(context, kScheme, handleSchemeRequest, this, nullptr);

    // Extensions hard-code the scheme of the browser they were built for into
    // CSS and HTML assets — LanguageTool's stylesheet asks for its fonts over
    // moz-extension://. The host is still the extension id, so the same handler
    // answers; without this every such asset is a failed request.
    for (const char *alias : { "moz-extension", "chrome-extension" })
        webkit_web_context_register_uri_scheme(context, alias, handleSchemeRequest, this, nullptr);

    // Extension pages need a real, storage-owning origin: "secure" keeps them
    // out of mixed-content downgrades, "cors-enabled" lets their own fetches
    // of extension resources work. Deliberately NOT registered as local or
    // no-access — both would strip the origin they need.
    WebKitSecurityManager *security = webkit_web_context_get_security_manager(context);
    for (const char *scheme : { kScheme, "moz-extension", "chrome-extension" }) {
        webkit_security_manager_register_uri_scheme_as_secure(security, scheme);
        webkit_security_manager_register_uri_scheme_as_cors_enabled(security, scheme);
    }
}

void WebExtensionManager::handleSchemeRequest(WebKitURISchemeRequest *request, gpointer userData)
{
    auto *self = static_cast<WebExtensionManager *>(userData);
    const QUrl url(QString::fromUtf8(webkit_uri_scheme_request_get_uri(request)));
    const QString extensionId = url.host();

    const auto fail = [request](const QString &message) {
        GError *error = g_error_new_literal(G_FILE_ERROR, G_FILE_ERROR_NOENT,
                                            message.toUtf8().constData());
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
    };

    const Entry *entry = self->entryFor(extensionId);
    if (!entry || !entry->enabled) {
        fail(QStringLiteral("no enabled extension %1").arg(extensionId));
        return;
    }

    QString relative = url.path();
    while (relative.startsWith(QLatin1Char('/')))
        relative.remove(0, 1);
    if (relative.isEmpty())
        relative = QStringLiteral("index.html");

    // An extension declaring background.scripts has no HTML of its own, so the
    // page that hosts them is synthesised here - the same thing Chrome and
    // Firefox do, down to the name.
    if (relative == QLatin1String("_generated_background_page.html")) {
        QString html = QStringLiteral("<!doctype html><meta charset=\"utf-8\">"
                                      "<title>%1</title>\n").arg(entry->extension.name().toHtmlEscaped());
        const QStringList scripts = entry->extension.backgroundScripts();
        for (const QString &script : scripts) {
            QString src = script;
            while (src.startsWith(QLatin1Char('/')))
                src.remove(0, 1);
            html += QStringLiteral("<script src=\"%1\"></script>\n").arg(src.toHtmlEscaped());
        }
        const QByteArray body = html.toUtf8();
        GInputStream *stream = g_memory_input_stream_new_from_data(
            g_memdup2(body.constData(), body.size()), body.size(), g_free);
        webkit_uri_scheme_request_finish(request, stream, body.size(), "text/html");
        g_object_unref(stream);
        return;
    }

    // Path traversal guard: resolve, then require the result to stay inside the
    // extension directory.
    const QString base = QDir(entry->extension.baseDir()).absolutePath();
    const QString resolved = QDir::cleanPath(QDir(base).absoluteFilePath(relative));
    if (!resolved.startsWith(base + QLatin1Char('/'))) {
        fail(QStringLiteral("path escapes the extension directory"));
        return;
    }

    // A page of the same extension may read anything it ships; everybody else
    // is held to web_accessible_resources.
    bool sameExtension = false;
    if (WebKitWebView *view = webkit_uri_scheme_request_get_web_view(request)) {
        if (const gchar *viewUri = webkit_web_view_get_uri(view)) {
            const QUrl requesting(QString::fromUtf8(viewUri));
            sameExtension = requesting.scheme() == QLatin1String(kScheme)
                && requesting.host() == extensionId;
        }
    }
    // The popup and options pages open as ordinary tabs, and at that moment the
    // view still reports the *previous* page's URL — a top-level navigation to
    // them is indistinguishable here from a web page fetching them. They are
    // therefore always served, which does mean a page could iframe an
    // extension's popup; extensions that care must not put secrets in one.
    const QString popup = entry->popupOverride.isEmpty() ? entry->extension.actionPopup()
                                                         : entry->popupOverride;
    const bool declaredPage = (!popup.isEmpty() && relative == popup)
        || (!entry->extension.optionsPage().isEmpty()
            && relative == entry->extension.optionsPage());

    if (!sameExtension && !declaredPage && !entry->extension.isWebAccessible(relative)) {
        fail(QStringLiteral("%1 is not web-accessible").arg(relative));
        return;
    }

    GError *error = nullptr;
    GFile *file = g_file_new_for_path(resolved.toUtf8().constData());
    GFileInputStream *stream = g_file_read(file, nullptr, &error);
    if (!stream) {
        g_object_unref(file);
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }

    const qint64 size = QFileInfo(resolved).size();
    const QByteArray mimeType = mimeTypeForPath(resolved).toUtf8();

    // Served through a response object rather than plain _finish() so the CORS
    // header can be set. An extension page is a different origin from the page
    // a content script runs in, and an ES module is *always* fetched in CORS
    // mode — so the loader pattern (a content script doing
    // `import(runtime.getURL("main.js"))`, which is how LanguageTool and many
    // bundled extensions start) is blocked outright without this, as is any
    // fetch() of an extension resource from a page world. Registering the
    // scheme as cors-enabled only permits the check; it does not answer it.
    WebKitURISchemeResponse *response =
        webkit_uri_scheme_response_new(G_INPUT_STREAM(stream), size);
    webkit_uri_scheme_response_set_content_type(response, mimeType.constData());
    webkit_uri_scheme_response_set_status(response, 200, "OK");

    SoupMessageHeaders *headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
    soup_message_headers_append(headers, "Access-Control-Allow-Origin", "*");
    // Only what a resource fetch needs; extension resources are read-only.
    soup_message_headers_append(headers, "Access-Control-Allow-Methods", "GET");
    webkit_uri_scheme_response_set_http_headers(response, headers);

    webkit_uri_scheme_request_finish_with_response(request, response);
    g_object_unref(response);
    g_object_unref(stream);
    g_object_unref(file);
}

// --- content-script installation --------------------------------------------

QString WebExtensionManager::worldNameFor(const QString &extensionId)
{
    return QLatin1String(kWorldPrefix) + sanitize(extensionId);
}

QString WebExtensionManager::handlerNameFor(const QString &extensionId, bool mainWorld)
{
    return (mainWorld ? QStringLiteral("atlExtPage_") : QStringLiteral("atlExt_"))
        + sanitize(extensionId);
}

QString WebExtensionManager::buildShim(const Entry &entry, const QString &context,
                                       const QString &handler) const
{
    QString shim = QString::fromUtf8(WebExtensionScripts::kApiShim);
    shim.replace(QLatin1String("@@ATL_EXT_ID@@"), jsonLiteral(entry.extension.id()));
    shim.replace(QLatin1String("@@ATL_MANIFEST@@"), compact(entry.extension.manifest()));
    shim.replace(QLatin1String("@@ATL_L10N@@"), compact(entry.extension.localeMessages()));
    shim.replace(QLatin1String("@@ATL_CONTEXT@@"), jsonLiteral(context));
    shim.replace(QLatin1String("@@ATL_HANDLER@@"), jsonLiteral(handler));
    return shim;
}

void WebExtensionManager::onScriptMessage(WebKitUserContentManager *, JSCValue *value,
                                          gpointer userData)
{
    auto *binding = static_cast<HandlerBinding *>(userData);
    if (!binding || !binding->manager || !value)
        return;
    gchar *json = jsc_value_to_string(value);
    if (json) {
        binding->manager->handleBridgeMessage(binding->extensionId, binding->page,
                                              binding->mainWorld, QString::fromUtf8(json));
        g_free(json);
    }
}

void WebExtensionManager::installIntoPage(WebKitUserContentManager *ucm, WPEWebPage *page)
{
    if (!ucm || !page)
        return;

    forgetPage(page);

    PageState state;
    state.ucm = ucm;

    for (const Entry &entry : m_entries) {
        if (!entry.enabled)
            continue;

        const WebExtension &extension = entry.extension;
        const QString extensionId = extension.id();
        const QByteArray world = worldNameFor(extensionId).toUtf8();

        // --- the extension's own pages (popup, options) ---
        {
            const QString handler = handlerNameFor(extensionId, true);
            auto *binding = new HandlerBinding{ this, extensionId, page, true };
            state.bindings.append(binding);

            const QByteArray detail =
                (QStringLiteral("script-message-received::") + handler).toUtf8();
            g_signal_connect(ucm, detail.constData(), G_CALLBACK(onScriptMessage), binding);
            webkit_user_content_manager_register_script_message_handler(
                ucm, handler.toUtf8().constData(), nullptr);
            state.handlers.append(qMakePair(handler, QString()));

            const QStringList allow{ QStringLiteral("%1://%2/*")
                                         .arg(QLatin1String(kScheme), extensionId) };
            QVector<QByteArray> storage;
            std::vector<const char *> allowList = patternVector(allow, storage);
            const QByteArray shim =
                buildShim(entry, QStringLiteral("content"), handler).toUtf8();
            WebKitUserScript *script = webkit_user_script_new(
                shim.constData(), WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, allowList.data(), nullptr);
            webkit_user_content_manager_add_script(ucm, script);
            state.scripts.append(script);
        }

        const QVector<ContentScriptDef> defs = extension.contentScripts();
        // Not just the manifest: browser.scripting.registerContentScripts can be
        // an extension's only injection route, and it still needs the isolated
        // world, the bridge handler and the shim set up below.
        if (defs.isEmpty() && entry.dynamicScripts.isEmpty())
            continue;

        // --- content scripts, in the extension's isolated world ---
        const QString handler = handlerNameFor(extensionId, false);
        auto *binding = new HandlerBinding{ this, extensionId, page, false };
        state.bindings.append(binding);

        const QByteArray detail = (QStringLiteral("script-message-received::") + handler).toUtf8();
        g_signal_connect(ucm, detail.constData(), G_CALLBACK(onScriptMessage), binding);
        webkit_user_content_manager_register_script_message_handler(
            ucm, handler.toUtf8().constData(), world.constData());
        state.handlers.append(qMakePair(handler, worldNameFor(extensionId)));

        QStringList union_;
        bool anyAllFrames = false;
        for (const ContentScriptDef &def : defs) {
            union_ << def.matches;
            anyAllFrames = anyAllFrames || def.allFrames;
        }
        for (const DynamicScript &script : entry.dynamicScripts) {
            union_ << script.matches;
            anyAllFrames = anyAllFrames || script.allFrames;
        }

        // The shim goes in first and at document-start, so a document-start
        // content script already finds browser.* defined.
        {
            QVector<QByteArray> storage;
            std::vector<const char *> allowList = patternVector(expandPatterns(union_), storage);
            const QByteArray shim = buildShim(entry, QStringLiteral("content"), handler).toUtf8();
            WebKitUserScript *script = webkit_user_script_new_for_world(
                shim.constData(),
                anyAllFrames ? WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES
                             : WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, world.constData(),
                allowList.data(), nullptr);
            webkit_user_content_manager_add_script(ucm, script);
            state.scripts.append(script);
        }

        applyDynamicScripts(entry, state, ucm);

        for (const ContentScriptDef &def : defs) {
            QVector<QByteArray> allowStorage, blockStorage;
            std::vector<const char *> allowList =
                patternVector(expandPatterns(def.matches), allowStorage);
            std::vector<const char *> blockList =
                patternVector(expandPatterns(def.excludeMatches), blockStorage);
            const char *const *block = def.excludeMatches.isEmpty() ? nullptr : blockList.data();

            const WebKitUserContentInjectedFrames frames =
                def.allFrames ? WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES
                              : WEBKIT_USER_CONTENT_INJECT_TOP_FRAME;
            // document_idle has no WebKit equivalent; document-end is the
            // closest thing and is what other WebKit ports map it to.
            const WebKitUserScriptInjectionTime when =
                def.runAt == QLatin1String("document_start")
                    ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START
                    : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END;

            for (const QString &relative : def.css) {
                QFile file(QDir(extension.baseDir()).filePath(relative));
                if (!file.open(QIODevice::ReadOnly)) {
                    qWarning() << "[WEBEXT]" << extensionId << "missing content CSS" << relative;
                    continue;
                }
                // Firefox substitutes __MSG_ placeholders in content-script CSS,
                // and extensions rely on it: LanguageTool's stylesheet builds
                // font URLs out of __MSG_@@extension_id__, which without this
                // request a literal "__MSG_@@extension_id__" host and fail.
                const QByteArray source =
                    extension.substituteMessages(QString::fromUtf8(file.readAll())).toUtf8();
                WebKitUserStyleSheet *sheet = webkit_user_style_sheet_new_for_world(
                    source.constData(), frames, WEBKIT_USER_STYLE_LEVEL_USER, world.constData(),
                    allowList.data(), block);
                webkit_user_content_manager_add_style_sheet(ucm, sheet);
                state.styleSheets.append(sheet);
            }

            for (const QString &relative : def.js) {
                QFile file(QDir(extension.baseDir()).filePath(relative));
                if (!file.open(QIODevice::ReadOnly)) {
                    qWarning() << "[WEBEXT]" << extensionId << "missing content script" << relative;
                    continue;
                }
                const QByteArray source = file.readAll();
                WebKitUserScript *script = webkit_user_script_new_for_world(
                    source.constData(), frames, when, world.constData(), allowList.data(), block);
                webkit_user_content_manager_add_script(ucm, script);
                state.scripts.append(script);
            }
        }
    }

    m_pages.insert(page, state);
    if (!m_pageOrder.contains(page))
        m_pageOrder.append(page);
}

void WebExtensionManager::forgetPage(WPEWebPage *page)
{
    auto it = m_pages.find(page);
    if (it == m_pages.end())
        return;

    PageState &state = it.value();
    if (state.ucm) {
        for (WebKitUserScript *script : state.scripts) {
            webkit_user_content_manager_remove_script(state.ucm, script);
            webkit_user_script_unref(script);
        }
        for (WebKitUserStyleSheet *sheet : state.styleSheets) {
            webkit_user_content_manager_remove_style_sheet(state.ucm, sheet);
            webkit_user_style_sheet_unref(sheet);
        }
        for (const QPair<QString, QString> &handler : state.handlers) {
            webkit_user_content_manager_unregister_script_message_handler(
                state.ucm, handler.first.toUtf8().constData(),
                handler.second.isEmpty() ? nullptr : handler.second.toUtf8().constData());
        }
    }
    qDeleteAll(state.bindings);
    m_pages.erase(it);
    m_pageOrder.removeAll(page);

    // Drop anything still routed to this page.
    for (auto pending = m_pendingMessages.begin(); pending != m_pendingMessages.end();) {
        if (pending.value().origin.page == page)
            pending = m_pendingMessages.erase(pending);
        else
            ++pending;
    }
    for (auto port = m_ports.begin(); port != m_ports.end();) {
        if (port.value().a.page == page || port.value().b.page == page)
            port = m_ports.erase(port);
        else
            ++port;
    }
}

// --- bridge -----------------------------------------------------------------

QJsonObject WebExtensionManager::senderInfo(const QString &extensionId,
                                            const ExtContext &origin) const
{
    QJsonObject sender{ { QStringLiteral("id"), extensionId } };
    if (!origin.page)
        return sender;

    sender.insert(QStringLiteral("url"), origin.page->url().toString());
    sender.insert(QStringLiteral("frameId"), 0);
    if (!origin.mainWorld) {
        sender.insert(QStringLiteral("tab"),
                      QJsonObject{ { QStringLiteral("id"), origin.page->tabId() },
                                   { QStringLiteral("url"), origin.page->url().toString() },
                                   { QStringLiteral("title"), origin.page->title() },
                                   { QStringLiteral("active"),
                                     m_host && m_host->extActiveTabId() == origin.page->tabId() } });
    }
    return sender;
}

bool WebExtensionManager::isExtensionPage(const QString &extensionId, WPEWebPage *page) const
{
    if (!page)
        return false;
    const QUrl url = page->url();
    return url.scheme() == QLatin1String(kScheme) && url.host() == extensionId;
}

QVector<WebExtensionManager::ExtContext>
WebExtensionManager::contextsFor(const QString &extensionId, const ExtContext &except) const
{
    QVector<ExtContext> contexts;

    const Entry *entry = entryFor(extensionId);
    if (entry && (entry->background || entry->backgroundView)) {
        const ExtContext background;
        if (!(background == except))
            contexts.append(background);
    }

    // runtime.sendMessage reaches the extension's own pages, never its content
    // scripts — those are addressed with tabs.sendMessage, exactly as in Chrome.
    for (WPEWebPage *page : m_pageOrder) {
        if (!isExtensionPage(extensionId, page))
            continue;
        const ExtContext context{ page, true };
        if (!(context == except))
            contexts.append(context);
    }
    return contexts;
}

bool WebExtensionManager::contentContextForTab(const QString &extensionId, int tabId,
                                               ExtContext *out) const
{
    for (WPEWebPage *page : m_pageOrder) {
        if (!page || page->tabId() != tabId)
            continue;
        const Entry *entry = entryFor(extensionId);
        if (!entry || !entry->extension.hasHostAccess(page->url()))
            return false;
        *out = ExtContext{ page, false };
        return true;
    }
    return false;
}

void WebExtensionManager::deliver(const QString &extensionId, const ExtContext &target,
                                  const QJsonObject &payload)
{
    const QString json = compact(payload);

    if (target.isBackground()) {
        Entry *entry = entryFor(extensionId);
        if (!entry)
            return;
        if (entry->backgroundView)
            entry->backgroundView->dispatch(json);
        else if (entry->background)
            entry->background->dispatch(json);
        return;
    }

    if (!m_pages.contains(target.page))
        return;
    WebKitWebView *view = target.page->webView();
    if (!view)
        return;

    const QByteArray script =
        QStringLiteral("if (typeof __atlExtBridge !== 'undefined') __atlExtBridge.dispatch(%1);")
            .arg(jsonLiteral(json))
            .toUtf8();
    const QByteArray world = worldNameFor(extensionId).toUtf8();
    webkit_web_view_evaluate_javascript(view, script.constData(), -1,
                                        target.mainWorld ? nullptr : world.constData(),
                                        nullptr, nullptr, nullptr, nullptr);
}

void WebExtensionManager::reply(const QString &extensionId, const ExtContext &target, int seq,
                                bool ok, const QJsonValue &value, const QString &error)
{
    if (seq <= 0)
        return; // a notify(), not a call() — nothing is waiting
    QJsonObject payload{ { QStringLiteral("type"), QStringLiteral("reply") },
                         { QStringLiteral("seq"), seq },
                         { QStringLiteral("ok"), ok },
                         { QStringLiteral("value"), value } };
    if (!ok)
        payload.insert(QStringLiteral("error"), error);
    deliver(extensionId, target, payload);
}

void WebExtensionManager::emitEvent(const QString &extensionId, const ExtContext &target,
                                    const QString &name, const QJsonArray &args)
{
    deliver(extensionId, target,
            QJsonObject{ { QStringLiteral("type"), QStringLiteral("event") },
                         { QStringLiteral("name"), name },
                         { QStringLiteral("args"), args } });
}

void WebExtensionManager::broadcastEvent(const QString &name, const QJsonArray &args)
{
    for (const Entry &entry : m_entries) {
        if (!entry.enabled)
            continue;
        const QString extensionId = entry.extension.id();
        if (entry.background || entry.backgroundView)
            emitEvent(extensionId, ExtContext(), name, args);
        for (WPEWebPage *page : m_pageOrder) {
            if (isExtensionPage(extensionId, page))
                emitEvent(extensionId, ExtContext{ page, true }, name, args);
        }
    }
}

// --- storage ----------------------------------------------------------------

QString WebExtensionManager::storagePath(const QString &extensionId, const QString &area) const
{
    return QStringLiteral("%1/%2/storage-%3.json")
        .arg(extensionDataDirectory(), extensionId, area);
}

QJsonObject WebExtensionManager::readStorage(const QString &extensionId, const QString &area) const
{
    const QString key = extensionId + QLatin1Char('/') + area;
    const auto cached = m_storageCache.constFind(key);
    if (cached != m_storageCache.constEnd())
        return cached.value();

    QJsonObject data;
    QFile file(storagePath(extensionId, area));
    if (file.open(QIODevice::ReadOnly))
        data = QJsonDocument::fromJson(file.readAll()).object();
    const_cast<WebExtensionManager *>(this)->m_storageCache.insert(key, data);
    return data;
}

void WebExtensionManager::writeStorage(const QString &extensionId, const QString &area,
                                       const QJsonObject &data)
{
    m_storageCache.insert(extensionId + QLatin1Char('/') + area, data);

    // storage.session is per-run by definition; never persist it.
    if (area == QLatin1String("session"))
        return;

    const QString path = storagePath(extensionId, area);
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "[WEBEXT]" << extensionId << "cannot write" << area
                   << "storage:" << file.errorString();
        return;
    }
    file.write(QJsonDocument(data).toJson(QJsonDocument::Compact));
}

// --- API dispatch -----------------------------------------------------------

void WebExtensionManager::handleBridgeMessage(const QString &extensionId, WPEWebPage *page,
                                              bool mainWorld, const QString &json)
{
    const QJsonObject payload = QJsonDocument::fromJson(json.toUtf8()).object();
    dispatchApiCall(extensionId, ExtContext{ page, mainWorld },
                    payload.value(QStringLiteral("seq")).toInt(),
                    payload.value(QStringLiteral("api")).toString(),
                    payload.value(QStringLiteral("args")).toArray());
}

void WebExtensionManager::dispatchApiCall(const QString &extensionId, const ExtContext &origin,
                                          int seq, const QString &api, const QJsonArray &args)
{
    Entry *entry = entryFor(extensionId);
    if (!entry || !entry->enabled)
        return;

    const auto ok = [&](const QJsonValue &value) { reply(extensionId, origin, seq, true, value); };
    const auto err = [&](const QString &message) {
        reply(extensionId, origin, seq, false, QJsonValue(), message);
    };
    const auto arg = [&args](int index) { return args.at(index); };

    // --- diagnostics ---
    if (api == QLatin1String("log")) {
        const QString level = arg(0).toString();
        const QString text = arg(1).toString();
        if (level == QLatin1String("error"))
            qWarning("[WEBEXT] %s: %s", qPrintable(extensionId), qPrintable(text));
        else if (level == QLatin1String("warn"))
            qWarning("[WEBEXT] %s: %s", qPrintable(extensionId), qPrintable(text));
        else
            qDebug("[WEBEXT] %s: %s", qPrintable(extensionId), qPrintable(text));
        return;
    }

    // --- storage ---
    if (api.startsWith(QLatin1String("storage."))) {
        const QString area = arg(0).toString();
        QJsonObject data = readStorage(extensionId, area);

        if (api == QLatin1String("storage.get")) {
            const QJsonValue keys = arg(1);
            if (keys.isNull() || keys.isUndefined()) {
                ok(data);
            } else if (keys.isString()) {
                QJsonObject result;
                if (data.contains(keys.toString()))
                    result.insert(keys.toString(), data.value(keys.toString()));
                ok(result);
            } else if (keys.isArray()) {
                QJsonObject result;
                const QJsonArray wanted = keys.toArray();
                for (const QJsonValue &key : wanted) {
                    if (data.contains(key.toString()))
                        result.insert(key.toString(), data.value(key.toString()));
                }
                ok(result);
            } else if (keys.isObject()) {
                // Object form supplies defaults for missing keys.
                QJsonObject result = keys.toObject();
                for (auto it = result.begin(); it != result.end(); ++it) {
                    if (data.contains(it.key()))
                        *it = data.value(it.key());
                }
                ok(result);
            } else {
                ok(QJsonObject());
            }
            return;
        }

        if (api == QLatin1String("storage.set")) {
            const QJsonObject items = arg(1).toObject();
            QJsonObject changes;
            for (auto it = items.constBegin(); it != items.constEnd(); ++it) {
                QJsonObject change;
                if (data.contains(it.key()))
                    change.insert(QStringLiteral("oldValue"), data.value(it.key()));
                change.insert(QStringLiteral("newValue"), it.value());
                changes.insert(it.key(), change);
                data.insert(it.key(), it.value());
            }
            writeStorage(extensionId, area, data);
            ok(QJsonValue());
            if (!changes.isEmpty()) {
                const QVector<ExtContext> targets = contextsFor(extensionId, ExtContext{ nullptr, true });
                for (const ExtContext &target : targets)
                    emitEvent(extensionId, target, QStringLiteral("storage.onChanged"),
                              QJsonArray{ changes, area });
                // Content scripts observe storage too.
                for (WPEWebPage *page : m_pageOrder) {
                    if (!isExtensionPage(extensionId, page))
                        emitEvent(extensionId, ExtContext{ page, false },
                                  QStringLiteral("storage.onChanged"), QJsonArray{ changes, area });
                }
            }
            return;
        }

        if (api == QLatin1String("storage.remove")) {
            const QJsonValue keys = arg(1);
            QStringList removed;
            if (keys.isString()) {
                removed << keys.toString();
            } else if (keys.isArray()) {
                const QJsonArray list = keys.toArray();
                for (const QJsonValue &key : list)
                    removed << key.toString();
            }
            QJsonObject changes;
            for (const QString &key : removed) {
                if (!data.contains(key))
                    continue;
                changes.insert(key, QJsonObject{ { QStringLiteral("oldValue"), data.value(key) } });
                data.remove(key);
            }
            writeStorage(extensionId, area, data);
            ok(QJsonValue());
            if (!changes.isEmpty()) {
                const QVector<ExtContext> targets = contextsFor(extensionId, ExtContext{ nullptr, true });
                for (const ExtContext &target : targets)
                    emitEvent(extensionId, target, QStringLiteral("storage.onChanged"),
                              QJsonArray{ changes, area });
            }
            return;
        }

        if (api == QLatin1String("storage.clear")) {
            writeStorage(extensionId, area, QJsonObject());
            ok(QJsonValue());
            return;
        }

        if (api == QLatin1String("storage.getBytesInUse")) {
            ok(QJsonDocument(data).toJson(QJsonDocument::Compact).size());
            return;
        }
    }

    // --- runtime ---
    if (api == QLatin1String("runtime.getPlatformInfo")) {
        ok(QJsonObject{ { QStringLiteral("os"), QStringLiteral("linux") },
                        { QStringLiteral("arch"), QStringLiteral("arm64") },
                        { QStringLiteral("nacl_arch"), QStringLiteral("arm") } });
        return;
    }
    if (api == QLatin1String("runtime.openOptionsPage")) {
        openOptionsPage(extensionId);
        ok(QJsonValue());
        return;
    }
    if (api == QLatin1String("runtime.reload")) {
        startBackground(*entry);
        ok(QJsonValue());
        return;
    }

    if (api == QLatin1String("runtime.sendMessage")) {
        const QString targetId = arg(0).toString();
        if (!targetId.isEmpty() && targetId != extensionId) {
            err(QStringLiteral("Messaging between extensions is not supported"));
            return;
        }
        const QVector<ExtContext> targets = contextsFor(extensionId, origin);
        if (targets.isEmpty()) {
            err(QStringLiteral("Could not establish connection. Receiving end does not exist."));
            return;
        }

        const QString token = QStringLiteral("m%1").arg(m_nextToken++);
        PendingMessage pending;
        pending.extensionId = extensionId;
        pending.origin = origin;
        pending.originSeq = seq;
        pending.outstanding = targets.size();
        m_pendingMessages.insert(token, pending);

        const QJsonObject sender = senderInfo(extensionId, origin);
        for (const ExtContext &target : targets) {
            emitEvent(extensionId, target, QStringLiteral("runtime.onMessage"),
                      QJsonArray{ arg(1), sender, token });
        }
        return;
    }

    if (api == QLatin1String("tabs.sendMessage")) {
        ExtContext target;
        if (!contentContextForTab(extensionId, arg(0).toInt(), &target)) {
            err(QStringLiteral("Could not establish connection. Receiving end does not exist."));
            return;
        }
        const QString token = QStringLiteral("m%1").arg(m_nextToken++);
        PendingMessage pending;
        pending.extensionId = extensionId;
        pending.origin = origin;
        pending.originSeq = seq;
        pending.outstanding = 1;
        m_pendingMessages.insert(token, pending);
        emitEvent(extensionId, target, QStringLiteral("runtime.onMessage"),
                  QJsonArray{ arg(1), senderInfo(extensionId, origin), token });
        return;
    }

    if (api == QLatin1String("runtime.onMessageResponse")) {
        const QString token = arg(0).toString();
        auto it = m_pendingMessages.find(token);
        if (it == m_pendingMessages.end())
            return;
        PendingMessage &pending = it.value();
        const bool hasResponse = arg(1).toBool();
        if (hasResponse && !pending.answered) {
            pending.answered = true;
            reply(pending.extensionId, pending.origin, pending.originSeq, true, arg(2));
        }
        if (--pending.outstanding <= 0) {
            if (!pending.answered) {
                // Every receiver declined to answer: Chrome resolves with
                // undefined rather than hanging the sender's promise.
                reply(pending.extensionId, pending.origin, pending.originSeq, true, QJsonValue());
            }
            m_pendingMessages.erase(it);
        }
        return;
    }

    // --- ports ---
    if (api == QLatin1String("runtime.connect")) {
        const QString portId = arg(0).toString();
        const QString name = arg(1).toString();
        const QJsonValue tabId = arg(2);

        ExtContext target;
        bool haveTarget = false;
        if (tabId.isDouble()) {
            haveTarget = contentContextForTab(extensionId, tabId.toInt(), &target);
        } else {
            const QVector<ExtContext> targets = contextsFor(extensionId, origin);
            if (!targets.isEmpty()) {
                target = targets.first();
                haveTarget = true;
            }
        }
        if (!haveTarget) {
            emitEvent(extensionId, origin, QStringLiteral("runtime.portDisconnect"),
                      QJsonArray{ portId });
            return;
        }

        PortRoute route;
        route.extensionId = extensionId;
        route.a = origin;
        route.b = target;
        route.connected = true;
        m_ports.insert(portId, route);
        emitEvent(extensionId, target, QStringLiteral("runtime.onConnect"),
                  QJsonArray{ portId, name, senderInfo(extensionId, origin) });
        return;
    }

    if (api == QLatin1String("runtime.portMessage") || api == QLatin1String("runtime.portDisconnect")) {
        const QString portId = arg(0).toString();
        auto it = m_ports.find(portId);
        if (it == m_ports.end())
            return;
        const PortRoute route = it.value();
        const ExtContext other = (route.a == origin) ? route.b : route.a;
        if (api == QLatin1String("runtime.portMessage")) {
            emitEvent(extensionId, other, QStringLiteral("runtime.portMessage"),
                      QJsonArray{ portId, arg(1) });
        } else {
            m_ports.erase(it);
            emitEvent(extensionId, other, QStringLiteral("runtime.portDisconnect"),
                      QJsonArray{ portId });
        }
        return;
    }

    // --- tabs / windows ---
    if (api == QLatin1String("tabs.query")) {
        ok(m_host ? m_host->extQueryTabs(arg(0).toObject()) : QJsonArray());
        return;
    }
    if (api == QLatin1String("tabs.get") || api == QLatin1String("tabs.getCurrent")) {
        const int tabId = api == QLatin1String("tabs.getCurrent")
            ? (origin.page ? origin.page->tabId() : -1)
            : arg(0).toInt();
        if (!m_host || tabId < 0) {
            ok(QJsonValue());
            return;
        }
        const QJsonArray tabs = m_host->extQueryTabs(QJsonObject());
        for (const QJsonValue &tab : tabs) {
            if (tab.toObject().value(QStringLiteral("id")).toInt() == tabId) {
                ok(tab);
                return;
            }
        }
        ok(QJsonValue());
        return;
    }
    if (api == QLatin1String("tabs.create") || api == QLatin1String("windows.create")) {
        if (!m_host) {
            err(QStringLiteral("No browser window is available"));
            return;
        }
        const QJsonObject properties = arg(0).toObject();
        QString url = properties.value(QStringLiteral("url")).toString();
        if (url.isEmpty())
            url = properties.value(QStringLiteral("tabId")).toString();
        // Relative URLs in tabs.create resolve against the extension.
        if (!url.isEmpty() && !url.contains(QLatin1String("://"))) {
            QString relative = url;
            while (relative.startsWith(QLatin1Char('/')))
                relative.remove(0, 1);
            url = QStringLiteral("%1://%2/%3").arg(QLatin1String(kScheme), extensionId, relative);
        }
        const int tabId = m_host->extCreateTab(url, properties.value(QStringLiteral("active"))
                                                        .toBool(true));
        ok(QJsonObject{ { QStringLiteral("id"), tabId }, { QStringLiteral("url"), url } });
        return;
    }
    if (api == QLatin1String("tabs.update")) {
        if (!m_host) {
            err(QStringLiteral("No browser window is available"));
            return;
        }
        const bool explicitId = arg(0).isDouble();
        const int tabId = explicitId ? arg(0).toInt() : m_host->extActiveTabId();
        const QJsonObject properties = (explicitId ? arg(1) : arg(0)).toObject();
        ok(m_host->extUpdateTab(tabId, properties));
        return;
    }
    if (api == QLatin1String("tabs.reload")) {
        if (!m_host) {
            err(QStringLiteral("No browser window is available"));
            return;
        }
        const int tabId = arg(0).isDouble() ? arg(0).toInt() : m_host->extActiveTabId();
        ok(m_host->extUpdateTab(tabId, QJsonObject{ { QStringLiteral("reload"), true } }));
        return;
    }
    if (api == QLatin1String("tabs.remove")) {
        if (!m_host) {
            err(QStringLiteral("No browser window is available"));
            return;
        }
        if (arg(0).isArray()) {
            const QJsonArray ids = arg(0).toArray();
            for (const QJsonValue &id : ids)
                m_host->extRemoveTab(id.toInt());
        } else {
            m_host->extRemoveTab(arg(0).toInt());
        }
        ok(QJsonValue());
        return;
    }
    if (api == QLatin1String("windows.getCurrent") || api == QLatin1String("windows.getAll")) {
        QJsonObject window{ { QStringLiteral("id"), 1 },
                            { QStringLiteral("focused"), true },
                            { QStringLiteral("incognito"), false },
                            { QStringLiteral("type"), QStringLiteral("normal") },
                            { QStringLiteral("state"), QStringLiteral("fullscreen") } };
        if (arg(0).toObject().value(QStringLiteral("populate")).toBool(false) && m_host)
            window.insert(QStringLiteral("tabs"), m_host->extQueryTabs(QJsonObject()));
        if (api == QLatin1String("windows.getAll"))
            ok(QJsonArray{ window });
        else
            ok(window);
        return;
    }
    if (api == QLatin1String("windows.update")) {
        ok(QJsonObject{ { QStringLiteral("id"), 1 } });
        return;
    }

    // --- action ---
    if (api.startsWith(QLatin1String("action."))) {
        const QJsonObject details = arg(0).toObject();
        if (api == QLatin1String("action.setBadgeText")) {
            entry->badgeText = details.value(QStringLiteral("text")).toString();
            Q_EMIT actionStateChanged(extensionId);
            ok(QJsonValue());
        } else if (api == QLatin1String("action.getBadgeText")) {
            ok(entry->badgeText);
        } else if (api == QLatin1String("action.setBadgeBackgroundColor")) {
            entry->badgeColor = details.value(QStringLiteral("color")).toVariant().toString();
            Q_EMIT actionStateChanged(extensionId);
            ok(QJsonValue());
        } else if (api == QLatin1String("action.getBadgeBackgroundColor")) {
            ok(entry->badgeColor);
        } else if (api == QLatin1String("action.setTitle")) {
            entry->title = details.value(QStringLiteral("title")).toString();
            Q_EMIT actionStateChanged(extensionId);
            ok(QJsonValue());
        } else if (api == QLatin1String("action.getTitle")) {
            ok(entry->title.isEmpty() ? entry->extension.actionTitle() : entry->title);
        } else if (api == QLatin1String("action.setPopup")) {
            entry->popupOverride = details.value(QStringLiteral("popup")).toString();
            Q_EMIT actionStateChanged(extensionId);
            ok(QJsonValue());
        } else if (api == QLatin1String("action.getPopup")) {
            ok(entry->popupOverride.isEmpty() ? entry->extension.actionPopup()
                                              : entry->popupOverride);
        } else if (api == QLatin1String("action.openPopup")) {
            openActionPopup(extensionId);
            ok(QJsonValue());
        } else {
            // setIcon / enable / disable: accepted, but Atlantic draws the
            // action from the manifest icon and never hides it.
            ok(QJsonValue());
        }
        return;
    }

    // --- permissions ---
    if (api == QLatin1String("permissions.getAll")) {
        ok(QJsonObject{
            { QStringLiteral("permissions"),
              QJsonArray::fromStringList(entry->extension.permissions()) },
            { QStringLiteral("origins"),
              QJsonArray::fromStringList(entry->extension.hostPermissions()) } });
        return;
    }
    if (api == QLatin1String("permissions.contains") || api == QLatin1String("permissions.request")) {
        // Everything an extension can have is granted at install time, so a
        // request succeeds exactly when the manifest already declared it.
        const QJsonObject wanted = arg(0).toObject();
        bool granted = true;
        const QJsonArray permissions = wanted.value(QStringLiteral("permissions")).toArray();
        for (const QJsonValue &permission : permissions)
            granted = granted && entry->extension.hasPermission(permission.toString());
        const QJsonArray origins = wanted.value(QStringLiteral("origins")).toArray();
        for (const QJsonValue &origin_ : origins)
            granted = granted && entry->extension.hostPermissions().contains(origin_.toString());
        ok(granted);
        return;
    }
    if (api == QLatin1String("permissions.remove")) {
        ok(false);
        return;
    }

    // --- notifications ---
    if (api == QLatin1String("notifications.create")) {
        // (notificationId?, options)
        const QJsonObject options = arg(1).isObject() ? arg(1).toObject() : arg(0).toObject();
        const QString notificationId = arg(1).isObject() && arg(0).isString()
            ? arg(0).toString()
            : QStringLiteral("n%1").arg(m_nextToken++);
        Q_EMIT notificationRequested(extensionId,
                                     options.value(QStringLiteral("title")).toString(),
                                     options.value(QStringLiteral("message")).toString());
        ok(notificationId);
        return;
    }
    if (api == QLatin1String("notifications.clear")) {
        ok(true);
        return;
    }
    if (api == QLatin1String("notifications.getAll")) {
        ok(QJsonObject());
        return;
    }

    // --- management (self only; enumerating other extensions is not offered) ---
    if (api == QLatin1String("management.getSelf")) {
        ok(QJsonObject{
            { QStringLiteral("id"), extensionId },
            { QStringLiteral("name"), entry->extension.name() },
            { QStringLiteral("version"), entry->extension.version() },
            { QStringLiteral("description"), entry->extension.description() },
            { QStringLiteral("enabled"), entry->enabled },
            { QStringLiteral("installType"), QStringLiteral("normal") },
            { QStringLiteral("mayDisable"), true },
            { QStringLiteral("type"), QStringLiteral("extension") },
            { QStringLiteral("hostPermissions"),
              QJsonArray::fromStringList(entry->extension.hostPermissions()) },
            { QStringLiteral("permissions"),
              QJsonArray::fromStringList(entry->extension.permissions()) } });
        return;
    }

    // --- contextMenus ---
    if (api.startsWith(QLatin1String("contextMenus.")) || api.startsWith(QLatin1String("menus."))) {
        const QString action = api.section(QLatin1Char('.'), 1);

        if (action == QLatin1String("create")) {
            const QJsonObject props = arg(0).toObject();
            MenuItemDef item;
            item.id = props.value(QStringLiteral("id")).toVariant().toString();
            if (item.id.isEmpty())
                item.id = QStringLiteral("item%1").arg(m_nextToken++);
            item.parentId = props.value(QStringLiteral("parentId")).toVariant().toString();
            item.title = props.value(QStringLiteral("title")).toString();
            item.type = props.value(QStringLiteral("type")).toString(QStringLiteral("normal"));
            item.checked = props.value(QStringLiteral("checked")).toBool(false);
            item.enabled = props.value(QStringLiteral("enabled")).toBool(true);
            item.visible = props.value(QStringLiteral("visible")).toBool(true);
            const QJsonArray contexts = props.value(QStringLiteral("contexts")).toArray();
            for (const QJsonValue &value : contexts)
                item.contexts << value.toString();
            const QJsonArray documents = props.value(QStringLiteral("documentUrlPatterns")).toArray();
            for (const QJsonValue &value : documents)
                item.documentUrlPatterns << value.toString();
            const QJsonArray targets = props.value(QStringLiteral("targetUrlPatterns")).toArray();
            for (const QJsonValue &value : targets)
                item.targetUrlPatterns << value.toString();

            // Creating an existing id replaces it, which is what extensions
            // rely on when they rebuild their menu on every startup.
            for (int i = entry->menuItems.size() - 1; i >= 0; --i) {
                if (entry->menuItems.at(i).id == item.id)
                    entry->menuItems.removeAt(i);
            }
            entry->menuItems.append(item);
            Q_EMIT contextMenuItemsChanged();
            ok(item.id);
            return;
        }

        if (action == QLatin1String("update")) {
            const QString id = arg(0).toVariant().toString();
            const QJsonObject props = arg(1).toObject();
            for (MenuItemDef &item : entry->menuItems) {
                if (item.id != id)
                    continue;
                if (props.contains(QStringLiteral("title")))
                    item.title = props.value(QStringLiteral("title")).toString();
                if (props.contains(QStringLiteral("enabled")))
                    item.enabled = props.value(QStringLiteral("enabled")).toBool(true);
                if (props.contains(QStringLiteral("visible")))
                    item.visible = props.value(QStringLiteral("visible")).toBool(true);
                if (props.contains(QStringLiteral("checked")))
                    item.checked = props.value(QStringLiteral("checked")).toBool(false);
                if (props.contains(QStringLiteral("contexts"))) {
                    item.contexts.clear();
                    const QJsonArray contexts = props.value(QStringLiteral("contexts")).toArray();
                    for (const QJsonValue &value : contexts)
                        item.contexts << value.toString();
                }
                break;
            }
            Q_EMIT contextMenuItemsChanged();
            ok(QJsonValue());
            return;
        }

        if (action == QLatin1String("remove")) {
            const QString id = arg(0).toVariant().toString();
            for (int i = entry->menuItems.size() - 1; i >= 0; --i) {
                if (entry->menuItems.at(i).id == id)
                    entry->menuItems.removeAt(i);
            }
            Q_EMIT contextMenuItemsChanged();
            ok(QJsonValue());
            return;
        }

        if (action == QLatin1String("removeAll")) {
            entry->menuItems.clear();
            Q_EMIT contextMenuItemsChanged();
            ok(QJsonValue());
            return;
        }
    }

    // --- scripting ---
    if (api == QLatin1String("scripting.executeScript")) {
        executeScript(extensionId, origin, seq, arg(0).toObject());
        return; // executeScript replies from its async callback
    }
    if (api == QLatin1String("tabs.executeScript")) {
        // MV2 shape: (tabId?, details). Normalise onto the MV3 injection.
        const bool explicitId = arg(0).isDouble();
        QJsonObject injection = (explicitId ? arg(1) : arg(0)).toObject();
        if (explicitId) {
            injection.insert(QStringLiteral("target"),
                             QJsonObject{ { QStringLiteral("tabId"), arg(0).toInt() } });
        }
        executeScript(extensionId, origin, seq, injection);
        return;
    }
    if (api == QLatin1String("scripting.insertCSS") || api == QLatin1String("scripting.removeCSS")) {
        insertCss(extensionId, arg(0).toObject(), api.endsWith(QLatin1String("removeCSS")));
        ok(QJsonValue());
        return;
    }
    if (api == QLatin1String("tabs.insertCSS") || api == QLatin1String("tabs.removeCSS")) {
        const bool explicitId = arg(0).isDouble();
        QJsonObject injection = (explicitId ? arg(1) : arg(0)).toObject();
        if (explicitId) {
            injection.insert(QStringLiteral("target"),
                             QJsonObject{ { QStringLiteral("tabId"), arg(0).toInt() } });
        }
        insertCss(extensionId, injection, api.endsWith(QLatin1String("removeCSS")));
        ok(QJsonValue());
        return;
    }
    if (api == QLatin1String("scripting.registerContentScripts")) {
        const QJsonArray scripts = arg(0).toArray();
        for (const QJsonValue &value : scripts) {
            const QJsonObject object = value.toObject();
            DynamicScript script;
            script.id = object.value(QStringLiteral("id")).toString();
            if (script.id.isEmpty())
                continue;
            const QJsonArray matches = object.value(QStringLiteral("matches")).toArray();
            for (const QJsonValue &match : matches)
                script.matches << match.toString();
            const QJsonArray excludes = object.value(QStringLiteral("excludeMatches")).toArray();
            for (const QJsonValue &match : excludes)
                script.excludeMatches << match.toString();
            const QJsonArray js = object.value(QStringLiteral("js")).toArray();
            for (const QJsonValue &file : js)
                script.js << file.toString();
            const QJsonArray css = object.value(QStringLiteral("css")).toArray();
            for (const QJsonValue &file : css)
                script.css << file.toString();
            const QString runAt = object.value(QStringLiteral("runAt")).toString();
            if (!runAt.isEmpty())
                script.runAt = runAt;
            script.allFrames = object.value(QStringLiteral("allFrames")).toBool(false);
            script.mainWorld = object.value(QStringLiteral("world")).toString()
                                   .compare(QLatin1String("MAIN"), Qt::CaseInsensitive) == 0;
            if (script.matches.isEmpty())
                continue;

            // Re-registering an id replaces it, as the API specifies.
            for (int i = entry->dynamicScripts.size() - 1; i >= 0; --i) {
                if (entry->dynamicScripts.at(i).id == script.id)
                    entry->dynamicScripts.removeAt(i);
            }
            entry->dynamicScripts.append(script);
        }
        // Registered scripts take effect on the next load of a matching page,
        // the same as a manifest-declared one.
        ok(QJsonValue());
        return;
    }
    if (api == QLatin1String("scripting.unregisterContentScripts")) {
        const QJsonObject filter = arg(0).toObject();
        const QJsonArray ids = filter.value(QStringLiteral("ids")).toArray();
        if (ids.isEmpty() && !filter.contains(QStringLiteral("ids"))) {
            entry->dynamicScripts.clear();
        } else {
            for (const QJsonValue &value : ids) {
                const QString id = value.toString();
                for (int i = entry->dynamicScripts.size() - 1; i >= 0; --i) {
                    if (entry->dynamicScripts.at(i).id == id)
                        entry->dynamicScripts.removeAt(i);
                }
            }
        }
        ok(QJsonValue());
        return;
    }
    if (api == QLatin1String("scripting.getRegisteredContentScripts")) {
        QJsonArray result;
        for (const DynamicScript &script : entry->dynamicScripts) {
            result.append(QJsonObject{
                { QStringLiteral("id"), script.id },
                { QStringLiteral("matches"), QJsonArray::fromStringList(script.matches) },
                { QStringLiteral("excludeMatches"),
                  QJsonArray::fromStringList(script.excludeMatches) },
                { QStringLiteral("js"), QJsonArray::fromStringList(script.js) },
                { QStringLiteral("css"), QJsonArray::fromStringList(script.css) },
                { QStringLiteral("runAt"), script.runAt },
                { QStringLiteral("allFrames"), script.allFrames },
                { QStringLiteral("world"), script.mainWorld ? QStringLiteral("MAIN")
                                                            : QStringLiteral("ISOLATED") } });
        }
        ok(result);
        return;
    }

    err(QStringLiteral("%1 is not implemented").arg(api));
}

// --- tab events -------------------------------------------------------------

void WebExtensionManager::notifyTabCreated(int tabId, const QString &url)
{
    broadcastEvent(QStringLiteral("tabs.onCreated"),
                   QJsonArray{ QJsonObject{ { QStringLiteral("id"), tabId },
                                            { QStringLiteral("url"), url },
                                            { QStringLiteral("windowId"), 1 } } });
}

void WebExtensionManager::notifyTabUpdated(int tabId, const QString &url, const QString &title,
                                           bool loading)
{
    QJsonObject changeInfo{
        { QStringLiteral("status"), loading ? QStringLiteral("loading") : QStringLiteral("complete") }
    };
    if (!url.isEmpty())
        changeInfo.insert(QStringLiteral("url"), url);
    if (!title.isEmpty())
        changeInfo.insert(QStringLiteral("title"), title);

    broadcastEvent(QStringLiteral("tabs.onUpdated"),
                   QJsonArray{ tabId, changeInfo,
                               QJsonObject{ { QStringLiteral("id"), tabId },
                                            { QStringLiteral("url"), url },
                                            { QStringLiteral("title"), title },
                                            { QStringLiteral("windowId"), 1 } } });
}

void WebExtensionManager::notifyTabActivated(int tabId)
{
    broadcastEvent(QStringLiteral("tabs.onActivated"),
                   QJsonArray{ QJsonObject{ { QStringLiteral("tabId"), tabId },
                                            { QStringLiteral("windowId"), 1 } } });
}

void WebExtensionManager::notifyTabRemoved(int tabId)
{
    broadcastEvent(QStringLiteral("tabs.onRemoved"),
                   QJsonArray{ tabId,
                               QJsonObject{ { QStringLiteral("windowId"), 1 },
                                            { QStringLiteral("isWindowClosing"), false } } });
}

// --- browser.scripting -------------------------------------------------------

WPEWebPage *WebExtensionManager::pageForTab(const QString &extensionId, int tabId) const
{
    const Entry *entry = entryFor(extensionId);
    if (!entry)
        return nullptr;

    for (WPEWebPage *page : m_pageOrder) {
        if (!page || page->tabId() != tabId)
            continue;
        // Same rule as a declared content script: no host access, no injection.
        // activeTab would widen this, but it needs a user gesture to hang off
        // and Atlantic has no extension action button to provide one yet.
        if (!entry->extension.hasHostAccess(page->url())
            && !isExtensionPage(extensionId, page)) {
            return nullptr;
        }
        return page;
    }
    return nullptr;
}

QString WebExtensionManager::gatherSources(const WebExtension &extension,
                                           const QJsonObject &injection,
                                           const QString &inlineKey, QString *error) const
{
    QStringList parts;

    const QJsonValue inlineValue = injection.value(inlineKey);
    if (inlineValue.isString())
        parts << inlineValue.toString();

    // MV2 spelled the single-file form "file"; MV3 takes a "files" array.
    QStringList files;
    const QJsonValue filesValue = injection.value(QStringLiteral("files"));
    if (filesValue.isArray()) {
        const QJsonArray array = filesValue.toArray();
        for (const QJsonValue &entry : array)
            files << entry.toString();
    }
    if (injection.value(QStringLiteral("file")).isString())
        files << injection.value(QStringLiteral("file")).toString();

    for (const QString &relative : files) {
        // Path traversal guard: an injected file must come from the package.
        const QString base = QDir(extension.baseDir()).absolutePath();
        const QString resolved = QDir::cleanPath(QDir(base).absoluteFilePath(relative));
        if (!resolved.startsWith(base + QLatin1Char('/'))) {
            *error = QStringLiteral("%1 is outside the extension").arg(relative);
            return QString();
        }
        QFile file(resolved);
        if (!file.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("could not read %1").arg(relative);
            return QString();
        }
        parts << QString::fromUtf8(file.readAll());
    }

    if (parts.isEmpty())
        *error = QStringLiteral("nothing to inject");
    return parts.join(QLatin1Char('\n'));
}

namespace {

// Carries the reply address across the async evaluate.
struct ScriptCallback {
    QPointer<WebExtensionManager> manager;
    QString extensionId;
    WPEWebPage *originPage = nullptr;
    bool originMainWorld = false;
    int seq = 0;
};

} // namespace

void WebExtensionManager::executeScript(const QString &extensionId, const ExtContext &origin,
                                        int seq, const QJsonObject &injection)
{
    Entry *entry = entryFor(extensionId);
    if (!entry) {
        reply(extensionId, origin, seq, false, QJsonValue(), QStringLiteral("unknown extension"));
        return;
    }

    const QJsonObject target = injection.value(QStringLiteral("target")).toObject();
    const int tabId = target.contains(QStringLiteral("tabId"))
        ? target.value(QStringLiteral("tabId")).toInt()
        : (m_host ? m_host->extActiveTabId() : -1);

    WPEWebPage *page = pageForTab(extensionId, tabId);
    if (!page || !page->webView()) {
        reply(extensionId, origin, seq, false, QJsonValue(),
              QStringLiteral("Cannot access contents of the tab. Extension manifest must "
                             "request permission to access the respective host."));
        return;
    }

    QString source;
    QString error;
    // MV3 passes a function plus args; the shim has already stringified it,
    // because a function cannot cross the bridge.
    const QString functionSource = injection.value(QStringLiteral("funcSource")).toString();
    if (!functionSource.isEmpty()) {
        const QJsonArray args = injection.value(QStringLiteral("args")).toArray();
        source = QStringLiteral("(%1).apply(null, %2);")
                     .arg(functionSource,
                          QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)));
    } else {
        source = gatherSources(entry->extension, injection, QStringLiteral("code"), &error);
        if (source.isEmpty()) {
            reply(extensionId, origin, seq, false, QJsonValue(), error);
            return;
        }
    }

    const bool mainWorld = injection.value(QStringLiteral("world")).toString()
                               .compare(QLatin1String("MAIN"), Qt::CaseInsensitive) == 0;
    const QByteArray world = worldNameFor(extensionId).toUtf8();
    const QByteArray code = source.toUtf8();

    auto *callback = new ScriptCallback{ this, extensionId, origin.page, origin.mainWorld, seq };
    webkit_web_view_evaluate_javascript(
        page->webView(), code.constData(), -1,
        mainWorld ? nullptr : world.constData(), nullptr, nullptr,
        +[](GObject *object, GAsyncResult *result, gpointer userData) {
            std::unique_ptr<ScriptCallback> callback(static_cast<ScriptCallback *>(userData));
            GError *error = nullptr;
            JSCValue *value = webkit_web_view_evaluate_javascript_finish(
                WEBKIT_WEB_VIEW(object), result, &error);

            if (!callback->manager) {
                if (value)
                    g_object_unref(value);
                if (error)
                    g_error_free(error);
                return;
            }
            const ExtContext origin{ callback->originPage, callback->originMainWorld };

            if (!value) {
                const QString message = error ? QString::fromUtf8(error->message)
                                              : QStringLiteral("script evaluation failed");
                if (error)
                    g_error_free(error);
                callback->manager->reply(callback->extensionId, origin, callback->seq, false,
                                         QJsonValue(), message);
                return;
            }

            // executeScript resolves with one InjectionResult per frame; we
            // only ever inject into the top frame, so there is exactly one.
            QJsonValue resultValue;
            if (gchar *json = jsc_value_to_json(value, 0)) {
                // Wrapped in an array so a bare string or number parses;
                // QJsonDocument will not take a naked scalar. The explicit
                // QByteArray keeps this out of pointer-arithmetic overloads.
                QByteArray wrapped;
                wrapped.append('[').append(json).append(']');
                resultValue = QJsonDocument::fromJson(wrapped).array().at(0);
                g_free(json);
            }
            g_object_unref(value);

            callback->manager->reply(
                callback->extensionId, origin, callback->seq, true,
                QJsonArray{ QJsonObject{ { QStringLiteral("frameId"), 0 },
                                         { QStringLiteral("result"), resultValue } } });
        },
        callback);
}

void WebExtensionManager::insertCss(const QString &extensionId, const QJsonObject &injection,
                                    bool remove)
{
    Entry *entry = entryFor(extensionId);
    if (!entry)
        return;

    const QJsonObject target = injection.value(QStringLiteral("target")).toObject();
    const int tabId = target.contains(QStringLiteral("tabId"))
        ? target.value(QStringLiteral("tabId")).toInt()
        : (m_host ? m_host->extActiveTabId() : -1);

    WPEWebPage *page = pageForTab(extensionId, tabId);
    if (!page)
        return;
    auto state = m_pages.find(page);
    if (state == m_pages.end() || !state->ucm)
        return;

    QString error;
    const QString css = gatherSources(entry->extension, injection, QStringLiteral("css"), &error);
    if (css.isEmpty())
        return;

    // Keyed by extension + content so removeCSS can find the same sheet again;
    // that is exactly how the API identifies it, since there is no handle.
    const QString key = extensionId + QLatin1Char('\n') + css;

    if (remove) {
        if (WebKitUserStyleSheet *sheet = state->insertedCss.take(key)) {
            webkit_user_content_manager_remove_style_sheet(state->ucm, sheet);
            state->styleSheets.removeAll(sheet);
            webkit_user_style_sheet_unref(sheet);
        }
        return;
    }

    if (state->insertedCss.contains(key))
        return; // already applied; inserting twice would double the specificity

    const QByteArray source = css.toUtf8();
    const QByteArray world = worldNameFor(extensionId).toUtf8();
    WebKitUserStyleSheet *sheet = webkit_user_style_sheet_new_for_world(
        source.constData(), WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_STYLE_LEVEL_USER, world.constData(), nullptr, nullptr);
    webkit_user_content_manager_add_style_sheet(state->ucm, sheet);
    state->styleSheets.append(sheet);
    state->insertedCss.insert(key, sheet);
}

void WebExtensionManager::applyDynamicScripts(const Entry &entry, PageState &state,
                                              WebKitUserContentManager *ucm)
{
    if (entry.dynamicScripts.isEmpty())
        return;

    const QString extensionId = entry.extension.id();
    const QByteArray world = worldNameFor(extensionId).toUtf8();

    for (const DynamicScript &script : entry.dynamicScripts) {
        QVector<QByteArray> allowStorage, blockStorage;
        std::vector<const char *> allowList =
            patternVector(expandPatterns(script.matches), allowStorage);
        std::vector<const char *> blockList =
            patternVector(expandPatterns(script.excludeMatches), blockStorage);
        const char *const *block = script.excludeMatches.isEmpty() ? nullptr : blockList.data();

        const WebKitUserContentInjectedFrames frames =
            script.allFrames ? WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES
                             : WEBKIT_USER_CONTENT_INJECT_TOP_FRAME;
        const WebKitUserScriptInjectionTime when =
            script.runAt == QLatin1String("document_start")
                ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START
                : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END;

        for (const QString &relative : script.css) {
            QFile file(QDir(entry.extension.baseDir()).filePath(relative));
            if (!file.open(QIODevice::ReadOnly))
                continue;
            const QByteArray source = file.readAll();
            WebKitUserStyleSheet *sheet = webkit_user_style_sheet_new_for_world(
                source.constData(), frames, WEBKIT_USER_STYLE_LEVEL_USER,
                script.mainWorld ? nullptr : world.constData(), allowList.data(), block);
            webkit_user_content_manager_add_style_sheet(ucm, sheet);
            state.styleSheets.append(sheet);
        }

        for (const QString &relative : script.js) {
            QFile file(QDir(entry.extension.baseDir()).filePath(relative));
            if (!file.open(QIODevice::ReadOnly))
                continue;
            const QByteArray source = file.readAll();
            WebKitUserScript *userScript = webkit_user_script_new_for_world(
                source.constData(), frames, when,
                script.mainWorld ? nullptr : world.constData(), allowList.data(), block);
            webkit_user_content_manager_add_script(ucm, userScript);
            state.scripts.append(userScript);
        }
    }
}

// --- browser.contextMenus ----------------------------------------------------

bool WebExtensionManager::menuItemMatches(const MenuItemDef &item, const QVariantMap &context)
{
    if (!item.visible || item.type == QLatin1String("separator"))
        return false;

    const QUrl pageUrl(context.value(QStringLiteral("pageUrl")).toString());
    const QString linkUrl = context.value(QStringLiteral("linkUrl")).toString();
    const QString srcUrl = context.value(QStringLiteral("srcUrl")).toString();
    const QString selectionText = context.value(QStringLiteral("selectionText")).toString();
    const bool editable = context.value(QStringLiteral("editable")).toBool();

    // Which contexts does what was pressed actually satisfy? "page" is the
    // fallback the API defines: it applies when nothing more specific does.
    QStringList active{ QStringLiteral("all") };
    if (!linkUrl.isEmpty())
        active << QStringLiteral("link");
    if (!srcUrl.isEmpty())
        active << context.value(QStringLiteral("mediaType"), QStringLiteral("image")).toString();
    if (!selectionText.isEmpty())
        active << QStringLiteral("selection");
    if (editable)
        active << QStringLiteral("editable");
    if (linkUrl.isEmpty() && srcUrl.isEmpty() && selectionText.isEmpty() && !editable)
        active << QStringLiteral("page");

    QStringList wanted = item.contexts;
    if (wanted.isEmpty())
        wanted << QStringLiteral("page"); // the API's default

    bool contextOk = false;
    for (const QString &candidate : wanted) {
        if (candidate == QLatin1String("all") || active.contains(candidate)) {
            contextOk = true;
            break;
        }
    }
    if (!contextOk)
        return false;

    if (!item.documentUrlPatterns.isEmpty()) {
        bool matched = false;
        for (const QString &pattern : item.documentUrlPatterns)
            matched = matched || MatchPattern::parse(pattern).matches(pageUrl);
        if (!matched)
            return false;
    }

    if (!item.targetUrlPatterns.isEmpty()) {
        const QUrl target(linkUrl.isEmpty() ? srcUrl : linkUrl);
        bool matched = false;
        for (const QString &pattern : item.targetUrlPatterns)
            matched = matched || MatchPattern::parse(pattern).matches(target);
        if (!matched)
            return false;
    }

    return true;
}

QVariantList WebExtensionManager::contextMenuItems(const QVariantMap &context) const
{
    QVariantList items;
    const QString selectionText = context.value(QStringLiteral("selectionText")).toString();

    for (const Entry &entry : m_entries) {
        if (!entry.enabled)
            continue;
        // An extension only gets to offer items where it could have run anyway.
        const QUrl pageUrl(context.value(QStringLiteral("pageUrl")).toString());
        if (pageUrl.isValid() && !entry.extension.hasHostAccess(pageUrl)
            && !entry.extension.hasPermission(QStringLiteral("activeTab"))) {
            continue;
        }

        for (const MenuItemDef &item : entry.menuItems) {
            if (!menuItemMatches(item, context))
                continue;

            // %s in a title is replaced with the selection, truncated the way
            // other browsers do so a long selection cannot blow up the panel.
            QString title = item.title;
            if (title.contains(QLatin1String("%s"))) {
                QString shown = selectionText.simplified();
                if (shown.size() > 32)
                    shown = shown.left(31) + QChar(0x2026);
                title.replace(QLatin1String("%s"), shown);
            }

            items.append(QVariantMap{
                { QStringLiteral("extensionId"), entry.extension.id() },
                { QStringLiteral("extensionName"), entry.extension.name() },
                { QStringLiteral("itemId"), item.id },
                { QStringLiteral("title"), title },
                { QStringLiteral("type"), item.type },
                { QStringLiteral("checked"), item.checked },
                { QStringLiteral("enabled"), item.enabled } });
        }
    }
    return items;
}

void WebExtensionManager::activateContextMenuItem(const QString &extensionId,
                                                  const QString &itemId,
                                                  const QVariantMap &context)
{
    Entry *entry = entryFor(extensionId);
    if (!entry || !entry->enabled)
        return;

    MenuItemDef *item = nullptr;
    for (MenuItemDef &candidate : entry->menuItems) {
        if (candidate.id == itemId) {
            item = &candidate;
            break;
        }
    }
    if (!item || !item->enabled)
        return;

    const bool wasChecked = item->checked;
    if (item->type == QLatin1String("checkbox"))
        item->checked = !item->checked;
    else if (item->type == QLatin1String("radio"))
        item->checked = true;

    QJsonObject info{
        { QStringLiteral("menuItemId"), item->id },
        { QStringLiteral("editable"), context.value(QStringLiteral("editable")).toBool() },
        { QStringLiteral("pageUrl"), context.value(QStringLiteral("pageUrl")).toString() }
    };
    if (!item->parentId.isEmpty())
        info.insert(QStringLiteral("parentMenuItemId"), item->parentId);
    for (const char *key : { "linkUrl", "srcUrl", "selectionText", "mediaType" }) {
        const QString value = context.value(QLatin1String(key)).toString();
        if (!value.isEmpty())
            info.insert(QLatin1String(key), value);
    }
    if (item->type == QLatin1String("checkbox") || item->type == QLatin1String("radio")) {
        info.insert(QStringLiteral("wasChecked"), wasChecked);
        info.insert(QStringLiteral("checked"), item->checked);
    }

    QJsonObject tab;
    const int tabId = m_host ? m_host->extActiveTabId() : -1;
    if (tabId >= 0) {
        tab.insert(QStringLiteral("id"), tabId);
        tab.insert(QStringLiteral("url"), context.value(QStringLiteral("pageUrl")).toString());
    }

    // Chrome fires this at the background context; content scripts never see it.
    emitEvent(extensionId, ExtContext(), QStringLiteral("contextMenus.onClicked"),
              QJsonArray{ info, tab });
}

void WebExtensionManager::notifyNavigation(int tabId, const QString &url, const QString &stage)
{
    if (url.isEmpty() || url == QLatin1String("about:blank"))
        return;

    // Only extensions that could see the page get to hear about it navigating.
    const QUrl target(url);
    for (const Entry &entry : m_entries) {
        if (!entry.enabled || (!entry.background && !entry.backgroundView))
            continue;
        if (!entry.extension.hasHostAccess(target)
            && !entry.extension.hasPermission(QStringLiteral("webNavigation"))) {
            continue;
        }
        emitEvent(entry.extension.id(), ExtContext(),
                  QStringLiteral("webNavigation.") + stage,
                  QJsonArray{ QJsonObject{
                      { QStringLiteral("tabId"), tabId },
                      { QStringLiteral("url"), url },
                      { QStringLiteral("frameId"), 0 },
                      { QStringLiteral("parentFrameId"), -1 },
                      { QStringLiteral("timeStamp"),
                        double(QDateTime::currentMSecsSinceEpoch()) } } });
    }
}
