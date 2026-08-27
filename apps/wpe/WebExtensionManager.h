/*
 * Atlantic Browser — WebExtension host.
 *
 * Owns the installed-extension registry, serves atlantic-extension:// URLs,
 * installs content scripts into per-extension isolated worlds, and implements
 * the browser.* APIs that the JS shim (WebExtensionScripts.h) calls into.
 *
 * Also a QAbstractListModel so the settings UI can list, toggle and remove
 * extensions without a second model class.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "WebExtension.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QString>
#include <QVector>

// Qt's `#define signals public` mangles a GDBus struct field of the same name;
// the GLib headers WebKit pulls in have to be seen with the keyword undefined.
#pragma push_macro("signals")
#undef signals
#include <wpe/webkit.h>
#pragma pop_macro("signals")

class WPEWebPage;
class WebExtensionBackground;

// The tab-facing half of browser.tabs, implemented by WPEWebContainer. Kept as
// an interface so the manager never has to include the container (and so the
// tab APIs are inert, rather than crashing, before the container exists).
class WebExtensionHost
{
public:
    virtual ~WebExtensionHost() = default;
    virtual QJsonArray extQueryTabs(const QJsonObject &query) = 0;
    virtual int extCreateTab(const QString &url, bool active) = 0;
    virtual bool extUpdateTab(int tabId, const QJsonObject &properties) = 0;
    virtual bool extRemoveTab(int tabId) = 0;
    virtual int extActiveTabId() const = 0;
};

class WebExtensionManager : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QString extensionsDirectory READ extensionsDirectory CONSTANT)

public:
    // Host of atlantic-extension:// URLs and the prefix of the isolated world
    // each extension's content scripts run in.
    static constexpr const char *kScheme = "atlantic-extension";
    static constexpr const char *kWorldPrefix = "atlantic-ext-";

    enum Roles {
        ExtensionIdRole = Qt::UserRole + 1,
        NameRole,
        VersionRole,
        DescriptionRole,
        EnabledRole,
        IconPathRole,
        PermissionsRole,
        HostPermissionsRole,
        PopupUrlRole,
        OptionsUrlRole,
        HomepageRole,
        HasBackgroundRole,
        WarningsRole
    };

    static WebExtensionManager *instance();

    // QAbstractListModel
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString lastError() const { return m_lastError; }
    QString extensionsDirectory() const;
    static QString extensionDataDirectory();

    // Reads the extensions directory and the enabled-state registry, then
    // starts the background context of every enabled extension. Safe to call
    // more than once; later calls are a full reload.
    Q_INVOKABLE void reload();
    // Accepts a directory holding a manifest.json, or a .zip/.xpi/.crx archive.
    Q_INVOKABLE bool install(const QString &path);
    Q_INVOKABLE bool uninstall(const QString &extensionId);
    Q_INVOKABLE void setExtensionEnabled(const QString &extensionId, bool enabled);
    Q_INVOKABLE QVariantMap extensionInfo(const QString &extensionId) const;
    // Opens the extension's popup (or options page) as a normal tab — Atlantic
    // has no anchored popup surface.
    Q_INVOKABLE void openActionPopup(const QString &extensionId);
    Q_INVOKABLE void openOptionsPage(const QString &extensionId);

    // --- engine side ---

    // Registers the atlantic-extension scheme on the default web context, with
    // the security traits extension origins need. Must be called before any
    // WebProcess spawns; WPEWebContainer does it from configureSandboxPaths().
    void registerUriScheme();

    // Installs every enabled extension's shim, content scripts and style sheets
    // into this page's user-content manager, and registers the bridge handlers.
    void installIntoPage(WebKitUserContentManager *ucm, WPEWebPage *page);
    void forgetPage(WPEWebPage *page);

    void setHost(WebExtensionHost *host) { m_host = host; }

    // Tab lifecycle, forwarded to browser.tabs.on* in every extension context.
    // Called by WPEWebContainer; no-ops when nothing is listening.
    void notifyTabCreated(int tabId, const QString &url);
    void notifyTabUpdated(int tabId, const QString &url, const QString &title, bool loading);
    void notifyTabActivated(int tabId);
    void notifyTabRemoved(int tabId);

    // Called by WebExtensionBackground for messages it does not handle itself.
    void handleBridgeMessage(const QString &extensionId, WPEWebPage *page, bool mainWorld,
                             const QString &json);

Q_SIGNALS:
    void countChanged();
    void lastErrorChanged();
    void extensionsChanged();
    // Routed to the browser UI: extensions cannot open tabs themselves.
    void openUrlRequested(const QString &url, bool inNewTab);
    void notificationRequested(const QString &extensionId, const QString &title,
                               const QString &message);
    // Badge/title changes for the toolbar entry of an extension action.
    void actionStateChanged(const QString &extensionId);

private:
    explicit WebExtensionManager(QObject *parent = nullptr);
    ~WebExtensionManager() override;

    // Where a bridge message came from, and where replies and events go back
    // to. A null page is the extension's background context; mainWorld tells
    // an extension page (popup/options, which runs the shim in the page's own
    // world) apart from a content script in the extension's isolated world.
    struct ExtContext {
        WPEWebPage *page = nullptr;
        bool mainWorld = false;

        bool isBackground() const { return page == nullptr; }
        bool operator==(const ExtContext &other) const
        {
            return page == other.page && mainWorld == other.mainWorld;
        }
    };

    struct Entry {
        WebExtension extension;
        bool enabled = true;
        QStringList warnings;
        WebExtensionBackground *background = nullptr;
        // browser.action state, per extension (we have one window).
        QString badgeText;
        QString badgeColor;
        QString title;
        QString popupOverride;
    };

    // Bound to a script-message handler; identifies which extension and which
    // world a message arrived from. Owned by the PageState that registered it.
    struct HandlerBinding {
        WebExtensionManager *manager = nullptr;
        QString extensionId;
        WPEWebPage *page = nullptr;
        bool mainWorld = false;
    };

    struct PageState {
        QVector<WebKitUserScript *> scripts;
        QVector<WebKitUserStyleSheet *> styleSheets;
        // handler name / world name, so they can be unregistered on reload.
        QVector<QPair<QString, QString>> handlers;
        QVector<HandlerBinding *> bindings;
        WebKitUserContentManager *ucm = nullptr;
    };

    struct PendingMessage {
        QString extensionId;
        ExtContext origin;
        int originSeq = 0;
        int outstanding = 0;
        bool answered = false;
    };

    struct PortRoute {
        QString extensionId;
        ExtContext a;
        ExtContext b;
        bool connected = false;
    };

    Entry *entryFor(const QString &extensionId);
    const Entry *entryFor(const QString &extensionId) const;
    int indexOf(const QString &extensionId) const;

    void loadRegistry();
    void saveRegistry() const;
    void startBackground(Entry &entry);
    void stopBackground(Entry &entry);
    void setLastError(const QString &error);

    // Bridge plumbing.
    void dispatchApiCall(const QString &extensionId, const ExtContext &origin, int seq,
                         const QString &api, const QJsonArray &args);
    void reply(const QString &extensionId, const ExtContext &target, int seq, bool ok,
               const QJsonValue &value, const QString &error = QString());
    void emitEvent(const QString &extensionId, const ExtContext &target, const QString &name,
                   const QJsonArray &args);
    void broadcastEvent(const QString &name, const QJsonArray &args);
    void deliver(const QString &extensionId, const ExtContext &target, const QJsonObject &payload);
    QJsonObject senderInfo(const QString &extensionId, const ExtContext &origin) const;
    // Every live context of an extension other than `except`.
    QVector<ExtContext> contextsFor(const QString &extensionId, const ExtContext &except) const;
    // The content-script context of a tab, or an invalid context if that tab
    // has no page or the extension has no access to it.
    bool contentContextForTab(const QString &extensionId, int tabId, ExtContext *out) const;
    bool isExtensionPage(const QString &extensionId, WPEWebPage *page) const;

    // browser.storage backing: one JSON file per (extension, area).
    QString storagePath(const QString &extensionId, const QString &area) const;
    QJsonObject readStorage(const QString &extensionId, const QString &area) const;
    void writeStorage(const QString &extensionId, const QString &area, const QJsonObject &data);

    // atlantic-extension:// request handling.
    static void handleSchemeRequest(WebKitURISchemeRequest *request, gpointer userData);

    static QString worldNameFor(const QString &extensionId);
    // Content scripts and extension pages post on separate channels: one
    // handler name may only be registered once, and the two live in different
    // script worlds.
    static QString handlerNameFor(const QString &extensionId, bool mainWorld);
    QString buildShim(const Entry &entry, const QString &context, const QString &handler) const;
    static void onScriptMessage(WebKitUserContentManager *ucm, JSCValue *value, gpointer userData);

    // QList, not QVector: Entry is large and not relocatable, and QVector's
    // erase() instantiates a memmove branch over it that trips -Wclass-memaccess.
    QList<Entry> m_entries;
    QHash<WPEWebPage *, PageState> m_pages;
    QVector<WPEWebPage *> m_pageOrder;
    QHash<QString, PendingMessage> m_pendingMessages;
    QHash<QString, PortRoute> m_ports;
    QHash<QString, QJsonObject> m_storageCache; // "<id>/<area>" -> object
    WebExtensionHost *m_host = nullptr;
    QString m_lastError;
    quint64 m_nextToken = 1;
    bool m_schemeRegistered = false;

    friend class WebExtensionBackground;
};
