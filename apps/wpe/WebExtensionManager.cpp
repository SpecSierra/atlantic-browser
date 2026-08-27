/*
 * Atlantic Browser — WebExtension host.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "WebExtensionManager.h"

#include "WebExtensionBackground.h"
#include "WebExtensionScripts.h"
#include "WPEWebPage.h"

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

#include <vector>

// Qt's `#define signals public` mangles a GDBus struct field of the same name;
// the GLib headers WebKit pulls in have to be seen with the keyword undefined.
#pragma push_macro("signals")
#undef signals
#include <jsc/jsc.h>
#pragma pop_macro("signals")

#include <private/qzipreader_p.h>

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
        if (pattern == QLatin1String("<all_urls>"))
            out << QStringLiteral("http://*/*") << QStringLiteral("https://*/*");
        else
            out << pattern;
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
        const QJsonObject state = registry.value(entry.extension.id()).toObject();
        if (state.contains(QStringLiteral("enabled")))
            entry.enabled = state.value(QStringLiteral("enabled")).toBool(true);
    }
}

void WebExtensionManager::saveRegistry() const
{
    QJsonObject registry;
    for (const Entry &entry : m_entries) {
        registry.insert(entry.extension.id(),
                        QJsonObject{ { QStringLiteral("enabled"), entry.enabled } });
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
        if (permissions.contains(QStringLiteral("contextMenus"))
            || permissions.contains(QStringLiteral("menus"))) {
            entry.warnings << QStringLiteral("Context-menu entries are not shown.");
        }

        m_entries.append(entry);
    }

    loadRegistry();
    for (Entry &entry : m_entries) {
        if (entry.enabled)
            startBackground(entry);
    }

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
        QZipReader zip(info.absoluteFilePath());
        if (!zip.isReadable() || !zip.extractAll(staging.path())) {
            setLastError(QStringLiteral("%1 is not a readable extension archive")
                             .arg(info.fileName()));
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

    if (enabled)
        startBackground(entry);
    else
        stopBackground(entry);

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

void WebExtensionManager::startBackground(Entry &entry)
{
    stopBackground(entry);
    if (!entry.extension.hasBackground())
        return;

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
    if (!entry.background)
        return;
    entry.background->deleteLater();
    entry.background = nullptr;
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

    // Extension pages need a real, storage-owning origin: "secure" keeps them
    // out of mixed-content downgrades, "cors-enabled" lets their own fetches
    // of extension resources work. Deliberately NOT registered as local or
    // no-access — both would strip the origin they need.
    WebKitSecurityManager *security = webkit_web_context_get_security_manager(context);
    webkit_security_manager_register_uri_scheme_as_secure(security, kScheme);
    webkit_security_manager_register_uri_scheme_as_cors_enabled(security, kScheme);
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
    webkit_uri_scheme_request_finish(request, G_INPUT_STREAM(stream), size, mimeType.constData());
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
        if (defs.isEmpty())
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
                const QByteArray source = file.readAll();
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
    if (entry && entry->background) {
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
        if (entry && entry->background)
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
        if (entry.background)
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
