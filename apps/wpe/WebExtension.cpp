/*
 * Atlantic Browser — WebExtension manifest model.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "WebExtension.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>

namespace {

QStringList toStringList(const QJsonValue &value)
{
    QStringList out;
    if (value.isString()) {
        out << value.toString();
    } else if (value.isArray()) {
        const QJsonArray array = value.toArray();
        for (const QJsonValue &entry : array) {
            if (entry.isString())
                out << entry.toString();
        }
    }
    return out;
}

// Glob match over the whole string, '*' only (patterns never contain '?').
bool globMatch(const QString &glob, const QString &text)
{
    if (glob == QLatin1String("*"))
        return true;
    // Iterative two-pointer wildcard match — no regex compilation per request.
    int g = 0, t = 0, star = -1, mark = 0;
    while (t < text.size()) {
        if (g < glob.size() && (glob.at(g) == text.at(t))) {
            ++g;
            ++t;
        } else if (g < glob.size() && glob.at(g) == QLatin1Char('*')) {
            star = g++;
            mark = t;
        } else if (star >= 0) {
            g = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (g < glob.size() && glob.at(g) == QLatin1Char('*'))
        ++g;
    return g == glob.size();
}

QString slugify(const QString &input)
{
    QString out;
    out.reserve(input.size());
    for (const QChar &c : input) {
        const QChar lower = c.toLower();
        if ((lower >= QLatin1Char('a') && lower <= QLatin1Char('z'))
            || (lower >= QLatin1Char('0') && lower <= QLatin1Char('9'))
            || lower == QLatin1Char('.') || lower == QLatin1Char('-')
            || lower == QLatin1Char('_')) {
            out.append(lower);
        } else if (!out.endsWith(QLatin1Char('-'))) {
            out.append(QLatin1Char('-'));
        }
    }
    while (out.startsWith(QLatin1Char('-')))
        out.remove(0, 1);
    while (out.endsWith(QLatin1Char('-')))
        out.chop(1);
    return out;
}

} // namespace

// --- MatchPattern -----------------------------------------------------------

MatchPattern MatchPattern::parse(const QString &pattern)
{
    MatchPattern p;
    p.m_source = pattern;

    if (pattern == QLatin1String("<all_urls>") || pattern == QLatin1String("*")) {
        p.m_allUrls = true;
        p.m_valid = true;
        return p;
    }

    const int schemeEnd = pattern.indexOf(QLatin1String("://"));
    if (schemeEnd <= 0)
        return p; // invalid: no scheme separator

    p.m_scheme = pattern.left(schemeEnd);
    if (p.m_scheme == QLatin1String("*"))
        p.m_scheme.clear(); // "*" means http or https

    const QString rest = pattern.mid(schemeEnd + 3);
    const int pathStart = rest.indexOf(QLatin1Char('/'));
    if (pathStart < 0) {
        // "https://example.com" with no path is tolerated as "/*".
        p.m_host = rest;
        p.m_path = QStringLiteral("/*");
    } else {
        p.m_host = rest.left(pathStart);
        p.m_path = rest.mid(pathStart);
    }

    if (p.m_host == QLatin1String("*"))
        p.m_host.clear();
    if (p.m_path.isEmpty())
        p.m_path = QStringLiteral("/*");

    p.m_valid = true;
    return p;
}

bool MatchPattern::matchesHost(const QString &host) const
{
    if (m_host.isEmpty())
        return true;
    if (m_host.startsWith(QLatin1String("*."))) {
        const QString suffix = m_host.mid(2);
        return host.compare(suffix, Qt::CaseInsensitive) == 0
            || host.endsWith(QLatin1Char('.') + suffix, Qt::CaseInsensitive);
    }
    return host.compare(m_host, Qt::CaseInsensitive) == 0;
}

bool MatchPattern::matches(const QUrl &url) const
{
    if (!m_valid || !url.isValid())
        return false;

    const QString scheme = url.scheme().toLower();
    if (m_allUrls)
        return scheme == QLatin1String("http") || scheme == QLatin1String("https")
            || scheme == QLatin1String("ws") || scheme == QLatin1String("wss")
            || scheme == QLatin1String("file") || scheme == QLatin1String("ftp");

    if (m_scheme.isEmpty()) {
        if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
            return false;
    } else if (scheme != m_scheme) {
        return false;
    }

    if (!matchesHost(url.host()))
        return false;

    QString path = url.path();
    if (path.isEmpty())
        path = QStringLiteral("/");
    const QString query = url.query();
    if (!query.isEmpty())
        path += QLatin1Char('?') + query;
    return globMatch(m_path, path);
}

// --- WebExtension -----------------------------------------------------------

QString WebExtension::deriveId(const QJsonObject &manifest)
{
    // Firefox-style explicit id wins — it is what the extension's own code and
    // any published options URLs expect.
    for (const char *key : { "browser_specific_settings", "applications" }) {
        const QJsonObject settings = manifest.value(QLatin1String(key)).toObject();
        const QString id = settings.value(QStringLiteral("gecko")).toObject()
                               .value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) {
            const QString slug = slugify(id);
            if (!slug.isEmpty())
                return slug;
        }
    }

    const QString name = manifest.value(QStringLiteral("name")).toString();
    QString slug = slugify(name);
    if (slug.isEmpty())
        slug = QStringLiteral("extension");
    if (slug.size() > 40)
        slug.truncate(40);
    // Hash the name only (not the version) so an upgrade keeps the same id,
    // and with it the same storage area and atlantic-extension:// origin.
    const QByteArray digest = QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha1);
    return slug + QLatin1Char('-') + QString::fromLatin1(digest.toHex().left(8));
}

bool WebExtension::loadFromDirectory(const QString &dir, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    QFile manifestFile(QDir(dir).filePath(QStringLiteral("manifest.json")));
    if (!manifestFile.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("no manifest.json in %1").arg(dir));

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return fail(QStringLiteral("manifest.json is not valid JSON: %1").arg(parseError.errorString()));

    const QJsonObject manifest = doc.object();
    m_manifest = manifest;
    m_baseDir = QDir(dir).absolutePath();

    m_name = manifest.value(QStringLiteral("name")).toString();
    if (m_name.isEmpty())
        return fail(QStringLiteral("manifest.json has no name"));
    m_version = manifest.value(QStringLiteral("version")).toString();
    m_description = manifest.value(QStringLiteral("description")).toString();
    m_homepageUrl = manifest.value(QStringLiteral("homepage_url")).toString();
    m_manifestVersion = manifest.value(QStringLiteral("manifest_version")).toInt(2);
    m_defaultLocale = manifest.value(QStringLiteral("default_locale")).toString();
    m_icons = manifest.value(QStringLiteral("icons")).toObject();
    m_id = deriveId(manifest);

    // MV2 lumps host patterns into "permissions"; MV3 splits them out. Keep the
    // two lists separate either way so the UI can show API permissions apart
    // from site access.
    const QStringList declared = toStringList(manifest.value(QStringLiteral("permissions")));
    for (const QString &permission : declared) {
        if (permission == QLatin1String("<all_urls>") || permission.contains(QLatin1String("://")))
            m_hostPermissions << permission;
        else
            m_permissions << permission;
    }
    m_hostPermissions << toStringList(manifest.value(QStringLiteral("host_permissions")));
    m_permissions << toStringList(manifest.value(QStringLiteral("optional_permissions")));
    m_permissions.removeDuplicates();
    m_hostPermissions.removeDuplicates();

    parseContentScripts(manifest);
    parseBackground(manifest);
    parseAction(manifest);

    // MV2: [ "img/*.png" ]. MV3: [ { "resources": [...], "matches": [...] } ].
    const QJsonValue war = manifest.value(QStringLiteral("web_accessible_resources"));
    if (war.isArray()) {
        const QJsonArray entries = war.toArray();
        for (const QJsonValue &entry : entries) {
            if (entry.isString())
                m_webAccessibleResources << entry.toString();
            else if (entry.isObject())
                m_webAccessibleResources << toStringList(entry.toObject().value(QStringLiteral("resources")));
        }
    }

    loadLocaleMessages();
    return true;
}

void WebExtension::parseContentScripts(const QJsonObject &manifest)
{
    const QJsonArray scripts = manifest.value(QStringLiteral("content_scripts")).toArray();
    for (const QJsonValue &value : scripts) {
        const QJsonObject entry = value.toObject();
        ContentScriptDef def;
        def.matches = toStringList(entry.value(QStringLiteral("matches")));
        def.excludeMatches = toStringList(entry.value(QStringLiteral("exclude_matches")));
        def.js = toStringList(entry.value(QStringLiteral("js")));
        def.css = toStringList(entry.value(QStringLiteral("css")));
        def.allFrames = entry.value(QStringLiteral("all_frames")).toBool(false);
        def.matchAboutBlank = entry.value(QStringLiteral("match_about_blank")).toBool(false);
        const QString runAt = entry.value(QStringLiteral("run_at")).toString();
        if (!runAt.isEmpty())
            def.runAt = runAt;
        if (def.matches.isEmpty() || (def.js.isEmpty() && def.css.isEmpty()))
            continue; // nothing to inject, or nowhere to inject it
        m_contentScripts.append(def);
    }
}

void WebExtension::parseBackground(const QJsonObject &manifest)
{
    const QJsonObject background = manifest.value(QStringLiteral("background")).toObject();
    if (background.isEmpty())
        return;

    m_backgroundScripts = toStringList(background.value(QStringLiteral("scripts")));
    m_backgroundPage = background.value(QStringLiteral("page")).toString();
    m_backgroundIsModule = background.value(QStringLiteral("type")).toString() == QLatin1String("module");

    const QString worker = background.value(QStringLiteral("service_worker")).toString();
    if (!worker.isEmpty()) {
        m_backgroundIsServiceWorker = true;
        m_backgroundScripts << worker;
    }
}

void WebExtension::parseAction(const QJsonObject &manifest)
{
    // MV3 renamed browser_action -> action; page_action is the narrower MV2
    // variant and we treat it the same way.
    for (const char *key : { "action", "browser_action", "page_action" }) {
        const QJsonObject action = manifest.value(QLatin1String(key)).toObject();
        if (action.isEmpty())
            continue;
        m_actionPopup = action.value(QStringLiteral("default_popup")).toString();
        m_actionTitle = action.value(QStringLiteral("default_title")).toString();
        const QJsonValue icon = action.value(QStringLiteral("default_icon"));
        if (icon.isObject() && m_icons.isEmpty())
            m_icons = icon.toObject();
        break;
    }

    const QJsonValue optionsUi = manifest.value(QStringLiteral("options_ui"));
    if (optionsUi.isObject())
        m_optionsPage = optionsUi.toObject().value(QStringLiteral("page")).toString();
    if (m_optionsPage.isEmpty())
        m_optionsPage = manifest.value(QStringLiteral("options_page")).toString();
}

void WebExtension::loadLocaleMessages()
{
    if (m_defaultLocale.isEmpty())
        return;

    // Prefer the UI language, then its bare language code, then the manifest's
    // declared default. Extensions ship locale dirs with '_' separators.
    QStringList candidates;
    const QString uiName = QLocale().name(); // e.g. "fr_FR"
    candidates << uiName << uiName.section(QLatin1Char('_'), 0, 0) << m_defaultLocale;

    for (const QString &candidate : candidates) {
        if (candidate.isEmpty())
            continue;
        QFile file(QDir(m_baseDir).filePath(
            QStringLiteral("_locales/%1/messages.json").arg(candidate)));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject table = QJsonDocument::fromJson(file.readAll()).object();
        // { "key": { "message": "...", "placeholders": {...} } } — the shim
        // only needs the message text plus its placeholder defaults.
        for (auto it = table.constBegin(); it != table.constEnd(); ++it) {
            const QJsonObject entry = it.value().toObject();
            QJsonObject flat;
            flat.insert(QStringLiteral("message"), entry.value(QStringLiteral("message")));
            if (entry.contains(QStringLiteral("placeholders")))
                flat.insert(QStringLiteral("placeholders"), entry.value(QStringLiteral("placeholders")));
            // Lower-cased: chrome.i18n.getMessage is case-insensitive on keys.
            m_localeMessages.insert(it.key().toLower(), flat);
        }
        if (!m_localeMessages.isEmpty())
            return;
    }
}

bool WebExtension::hasHostAccess(const QUrl &url) const
{
    for (const QString &pattern : m_hostPermissions) {
        if (MatchPattern::parse(pattern).matches(url))
            return true;
    }
    for (const ContentScriptDef &def : m_contentScripts) {
        for (const QString &pattern : def.matches) {
            if (MatchPattern::parse(pattern).matches(url))
                return true;
        }
    }
    return false;
}

bool WebExtension::isWebAccessible(const QString &path) const
{
    const QString normalized = path.startsWith(QLatin1Char('/')) ? path.mid(1) : path;
    for (const QString &pattern : m_webAccessibleResources) {
        const QString candidate = pattern.startsWith(QLatin1Char('/')) ? pattern.mid(1) : pattern;
        if (globMatch(candidate, normalized))
            return true;
    }
    return false;
}

QString WebExtension::bestIconPath() const
{
    int bestSize = -1;
    QString best;
    for (auto it = m_icons.constBegin(); it != m_icons.constEnd(); ++it) {
        const int size = it.key().toInt();
        if (size > bestSize && it.value().isString()) {
            bestSize = size;
            best = it.value().toString();
        }
    }
    return best;
}
