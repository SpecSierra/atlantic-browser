/*
 * Atlantic Browser — WebExtension manifest model.
 *
 * Parses the manifest.json of an unpacked extension (MV2 and the MV3 subset we
 * support) into a plain value object. Nothing here touches WebKit; the manager
 * turns these into user scripts, scheme responses and API permissions.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

// A Chrome/Firefox match pattern: <scheme>://<host><path>, plus the two
// specials "<all_urls>" and "*". WebKit's UserContentURLPattern understands the
// same syntax, so the raw strings are handed straight to
// webkit_user_script_new_for_world(); this class exists for the checks we have
// to make ourselves (host permissions, tabs.sendMessage targeting).
class MatchPattern
{
public:
    static MatchPattern parse(const QString &pattern);

    bool isValid() const { return m_valid; }
    bool matches(const QUrl &url) const;
    QString source() const { return m_source; }

private:
    bool matchesHost(const QString &host) const;

    QString m_source;
    QString m_scheme;   // empty == any
    QString m_host;     // empty == any; may start with "*." for subdomains
    QString m_path = QStringLiteral("/*");
    bool m_allUrls = false;
    bool m_valid = false;
};

struct ContentScriptDef {
    QStringList matches;
    QStringList excludeMatches;
    QStringList js;
    QStringList css;
    QString runAt = QStringLiteral("document_idle");
    bool allFrames = false;
    bool matchAboutBlank = false;
};

class WebExtension
{
public:
    // Reads <dir>/manifest.json. Returns false and fills `error` on anything
    // that would leave us with a half-loaded extension.
    bool loadFromDirectory(const QString &dir, QString *error);

    // Stable across version bumps: the gecko id when the manifest declares one,
    // otherwise a slug of the name plus a hash of it. Used as the directory
    // name and as the host of atlantic-extension:// URLs, so it is always
    // restricted to [a-z0-9._-].
    // `resolvedName` is the manifest name after __MSG_ substitution; pass it
    // so a localized manifest does not derive its id from "__MSG_appName__".
    static QString deriveId(const QJsonObject &manifest,
                            const QString &resolvedName = QString());
    // The id an extension declaring this gecko id will get. Lets the store tell
    // whether an AMO add-on is already installed without downloading it.
    static QString idForGeckoId(const QString &geckoId);
    // The id an extension with this name and no gecko id will get. Only a guess
    // from outside — AMO's display name is not guaranteed to be the manifest
    // name — so use it for hints, never for anything destructive.
    static QString idForName(const QString &name);

    QString id() const { return m_id; }
    QString name() const { return m_name; }
    QString version() const { return m_version; }
    QString description() const { return m_description; }
    QString homepageUrl() const { return m_homepageUrl; }
    int manifestVersion() const { return m_manifestVersion; }
    QString baseDir() const { return m_baseDir; }
    QJsonObject manifest() const { return m_manifest; }

    QStringList permissions() const { return m_permissions; }
    QStringList hostPermissions() const { return m_hostPermissions; }
    bool hasPermission(const QString &name) const { return m_permissions.contains(name); }
    // True when a host permission (or an <all_urls>-style content script match)
    // covers `url`. "activeTab" is not considered here — the manager grants
    // that per user gesture.
    bool hasHostAccess(const QUrl &url) const;

    QVector<ContentScriptDef> contentScripts() const { return m_contentScripts; }

    // Background context. MV3 service workers are run as plain background
    // scripts: we have no service-worker lifecycle, so `backgroundScripts()`
    // carries the worker file and `backgroundIsServiceWorker()` records that
    // the manifest asked for one (surfaced in the UI as a caveat).
    QStringList backgroundScripts() const { return m_backgroundScripts; }
    QString backgroundPage() const { return m_backgroundPage; }
    bool backgroundIsServiceWorker() const { return m_backgroundIsServiceWorker; }
    bool backgroundIsModule() const { return m_backgroundIsModule; }
    bool hasBackground() const { return !m_backgroundScripts.isEmpty() || !m_backgroundPage.isEmpty(); }

    QString actionPopup() const { return m_actionPopup; }
    QString actionTitle() const { return m_actionTitle; }
    QString optionsPage() const { return m_optionsPage; }
    // Largest declared icon, as a path relative to baseDir(); empty if none.
    QString bestIconPath() const;

    QStringList webAccessibleResources() const { return m_webAccessibleResources; }
    // MV3 gates web-accessible resources on the requesting page; MV2 exposes
    // them to everyone. A resource not listed is still reachable from the
    // extension's own pages and content scripts.
    bool isWebAccessible(const QString &path) const;

    QString defaultLocale() const { return m_defaultLocale; }
    // Flattened <locale>/messages.json: { "key": "resolved string" }. Loaded
    // once at parse time; the shim does substitution in JS.
    QJsonObject localeMessages() const { return m_localeMessages; }

private:
    void parseContentScripts(const QJsonObject &manifest);
    void parseBackground(const QJsonObject &manifest);
    void parseAction(const QJsonObject &manifest);
    void loadLocaleMessages();
public:
    // Public because injected CSS needs the same substitution: Firefox
    // replaces __MSG_ placeholders in content-script stylesheets, and
    // extensions use __MSG_@@extension_id__ to build asset URLs there.
    QString substituteMessages(const QString &text) const { return resolveMessages(text); }
private:
    // Resolves a "__MSG_key__" placeholder against localeMessages(). Manifest
    // fields (name, description, action title) may be written that way, and an
    // unresolved one otherwise reaches the UI verbatim.
    QString resolveMessages(const QString &text) const;

    QString m_id;
    QString m_name;
    QString m_version;
    QString m_description;
    QString m_homepageUrl;
    QString m_baseDir;
    int m_manifestVersion = 2;
    QJsonObject m_manifest;

    QStringList m_permissions;
    QStringList m_hostPermissions;
    QVector<ContentScriptDef> m_contentScripts;

    QStringList m_backgroundScripts;
    QString m_backgroundPage;
    bool m_backgroundIsServiceWorker = false;
    bool m_backgroundIsModule = false;

    QString m_actionPopup;
    QString m_actionTitle;
    QString m_optionsPage;
    QJsonObject m_icons;

    QStringList m_webAccessibleResources;
    QString m_defaultLocale;
    QJsonObject m_localeMessages;
};
