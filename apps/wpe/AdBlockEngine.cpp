/*
 * Copyright (c) 2026
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "AdBlockEngine.h"
#include "WPEWebPage.h"
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVector>

bool AdBlockEngine::s_enabled = true;

AdBlockEngine& AdBlockEngine::instance()
{
    static AdBlockEngine inst;
    return inst;
}

AdBlockEngine::~AdBlockEngine()
{
    if (m_engine) atlantic_adblock_destroy(m_engine);
}

bool AdBlockEngine::loadFromCache(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "[ADBLOCK] engine cache not found:" << path;
        return false;
    }
    QByteArray data = f.readAll();
    m_engine = atlantic_adblock_create_from_cache(
        reinterpret_cast<const uint8_t*>(data.constData()),
        static_cast<size_t>(data.size()));
    if (!m_engine) {
        qWarning() << "[ADBLOCK] failed to deserialize engine cache";
        return false;
    }
    qInfo() << "[ADBLOCK] engine loaded from" << (data.size() / 1024) << "KB cache";
    return true;
}

bool AdBlockEngine::loadResources(const QString& path)
{
    if (!m_engine) return false;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "[ADBLOCK] scriptlet resources not found:" << path;
        return false;
    }
    QByteArray data = f.readAll();
    const bool ok = atlantic_adblock_use_resources_json(
        m_engine,
        reinterpret_cast<const uint8_t*>(data.constData()),
        static_cast<size_t>(data.size()));
    if (ok)
        qInfo() << "[ADBLOCK] scriptlet resources loaded from" << (data.size() / 1024) << "KB json";
    else
        qWarning() << "[ADBLOCK] scriptlet resources failed to parse:" << path;
    return ok;
}

QString AdBlockEngine::genericHides(const QUrl& url, const QByteArray& classes, const QByteArray& ids)
{
    if (!m_engine || !s_enabled) return QString();
    if (classes.isEmpty() && ids.isEmpty()) return QString();

    QByteArray urlUtf8 = url.toString().toUtf8();
    char* sels = atlantic_adblock_get_generic_hides(
        m_engine, urlUtf8.constData(), classes.constData(), ids.constData());
    if (!sels) return QString();
    QString result = QString::fromUtf8(sels);
    atlantic_adblock_free_string(sels);
    return result;
}

// Same relatedness rule as the WebProcess extension: equal hosts or one a
// dotted suffix of the other count as first-party.
static bool hostsRelated(const QString& a, const QString& b)
{
    if (a.isEmpty() || b.isEmpty()) return false;
    if (a.compare(b, Qt::CaseInsensitive) == 0) return true;
    if (a.length() > b.length())
        return a.endsWith(QLatin1Char('.') + b, Qt::CaseInsensitive);
    if (b.length() > a.length())
        return b.endsWith(QLatin1Char('.') + a, Qt::CaseInsensitive);
    return false;
}

bool AdBlockEngine::shouldBlockPopup(const QUrl& pageUrl, const QUrl& popupUrl)
{
    if (!m_engine || !s_enabled) return false;
    if (!popupUrl.scheme().startsWith(QLatin1String("http"))) return false;

    const QByteArray src = pageUrl.toString().toUtf8();
    const QByteArray req = popupUrl.toString().toUtf8();
    const int thirdParty = hostsRelated(pageUrl.host(), popupUrl.host()) ? 0 : 1;

    MatchResult r = atlantic_adblock_match_network(
        m_engine, src.constData(), req.constData(), "document", thirdParty);
    // A popup can't carry a surrogate redirect; only a plain match blocks.
    const bool block = r.matched && !r.redirect;
    atlantic_adblock_free_match_result(r);
    return block;
}

// Hosts whose cosmetic sheet is already installed on a given content manager,
// stored on the manager itself so the set dies with it.
static const char* kInstalledHostsKey = "atlantic-adblock-cosmetic-hosts";
// Scriptlet user scripts we installed, kept so resetCosmetics can remove them
// individually — the manager carries many non-adblock scripts (bridges), so
// remove_all_scripts is off limits.
static const char* kInstalledScriptsKey = "atlantic-adblock-scriptlets";

static QVector<WebKitUserScript*>* installedScripts(WebKitUserContentManager* ucm, bool create)
{
    auto* scripts = static_cast<QVector<WebKitUserScript*>*>(
        g_object_get_data(G_OBJECT(ucm), kInstalledScriptsKey));
    if (!scripts && create) {
        scripts = new QVector<WebKitUserScript*>();
        g_object_set_data_full(G_OBJECT(ucm), kInstalledScriptsKey, scripts,
                               [](gpointer p) {
                                   auto* v = static_cast<QVector<WebKitUserScript*>*>(p);
                                   for (WebKitUserScript* s : *v)
                                       webkit_user_script_unref(s);
                                   delete v;
                               });
    }
    return scripts;
}

void AdBlockEngine::installCosmetics(WebKitUserContentManager* ucm, const QUrl& url)
{
    if (!m_engine || !s_enabled || !ucm) return;

    const QString host = url.host();
    if (host.isEmpty() || !url.scheme().startsWith(QLatin1String("http"))) return;

    auto* hosts = static_cast<QSet<QString>*>(
        g_object_get_data(G_OBJECT(ucm), kInstalledHostsKey));
    if (hosts && hosts->contains(host)) return;

    QByteArray urlUtf8 = url.toString().toUtf8();
    CosmeticResult cr = atlantic_adblock_get_cosmetic(m_engine, urlUtf8.constData());

    QString css;
    int ruleCount = 0;
    if (cr.hide_selectors && *cr.hide_selectors) {
        // One rule per selector: the list can contain uBO procedural selectors
        // (e.g. ":has-text(...)") that are NOT valid CSS. In a stylesheet an
        // invalid rule is dropped individually, but an invalid selector inside
        // a comma-joined group would discard the whole group — so never group.
        // (The Rust side newline-separates selectors for the same reason.)
        const QStringList sels =
            QString::fromUtf8(cr.hide_selectors).split(QLatin1Char('\n'), QString::SkipEmptyParts);
        for (const QString& s : sels) {
            const QString t = s.trimmed();
            if (t.isEmpty()) continue;
            css += t + QLatin1String("{display:none!important}\n");
            ++ruleCount;
        }
    }
    // generated_css last: it is untrusted list content and a stray brace in it
    // must not be able to swallow the hide rules above.
    if (cr.generated_css && *cr.generated_css)
        css += QString::fromUtf8(cr.generated_css) + QLatin1Char('\n');

    const QString scriptlets = (cr.injected_script && *cr.injected_script)
        ? QString::fromUtf8(cr.injected_script) : QString();

    atlantic_adblock_free_cosmetic(cr);

    // Scope to this exact host (each subdomain gets its own sheet on first
    // visit), all frames, user level so !important beats page styles.
    const QByteArray hostUtf8 = host.toUtf8();
    const QByteArray httpPat  = "http://"  + hostUtf8 + "/*";
    const QByteArray httpsPat = "https://" + hostUtf8 + "/*";
    const char* allowList[] = { httpPat.constData(), httpsPat.constData(), nullptr };

    if (!css.isEmpty()) {
        WebKitUserStyleSheet* sheet = webkit_user_style_sheet_new(
            css.toUtf8().constData(),
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_STYLE_LEVEL_USER,
            allowList, nullptr);
        webkit_user_content_manager_add_style_sheet(ucm, sheet);
        webkit_user_style_sheet_unref(sheet);
        qInfo() << "[ADBLOCK] cosmetic sheet installed for" << host << "-" << ruleCount << "rules";
    }

    // ##+js(...) scriptlets as a document-start user script (they must run
    // before page scripts to be effective — the old post-load injection was a
    // no-op against anti-adblock). Installed at load-committed: the very first
    // navigation to a host may race document creation, but every later one
    // gets it at true document start.
    if (!scriptlets.isEmpty()) {
        WebKitUserScript* script = webkit_user_script_new(
            scriptlets.toUtf8().constData(),
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
            allowList, nullptr);
        webkit_user_content_manager_add_script(ucm, script);
        installedScripts(ucm, true)->append(script); // keep the ref for removal
        qInfo() << "[ADBLOCK] scriptlets installed for" << host;
    }

    if (!hosts) {
        hosts = new QSet<QString>();
        g_object_set_data_full(G_OBJECT(ucm), kInstalledHostsKey, hosts,
                               [](gpointer p) { delete static_cast<QSet<QString>*>(p); });
    }
    hosts->insert(host); // also cache "no cosmetics" hosts
}

void AdBlockEngine::resetCosmetics(WebKitUserContentManager* ucm)
{
    if (!ucm) return;
    // Cosmetic sheets are the only user style sheets we install; scriptlets
    // must be removed one by one (the manager carries other user scripts).
    webkit_user_content_manager_remove_all_style_sheets(ucm);
    if (auto* scripts = installedScripts(ucm, false)) {
        for (WebKitUserScript* s : *scripts)
            webkit_user_content_manager_remove_script(ucm, s);
    }
    g_object_set_data(G_OBJECT(ucm), kInstalledScriptsKey, nullptr);
    g_object_set_data(G_OBJECT(ucm), kInstalledHostsKey, nullptr);
}

bool AdBlockEngine::isEnabled()
{
    return s_enabled;
}

void AdBlockEngine::setEnabled(bool enabled)
{
    s_enabled = enabled;
}
