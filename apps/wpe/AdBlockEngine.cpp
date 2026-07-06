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

// Hosts whose cosmetic sheet is already installed on a given content manager,
// stored on the manager itself so the set dies with it.
static const char* kInstalledHostsKey = "atlantic-adblock-cosmetic-hosts";

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

    atlantic_adblock_free_cosmetic(cr);

    if (!css.isEmpty()) {
        // Scope to this exact host (each subdomain gets its own sheet on first
        // visit), all frames, user level so !important beats page styles.
        const QByteArray hostUtf8 = host.toUtf8();
        const QByteArray httpPat  = "http://"  + hostUtf8 + "/*";
        const QByteArray httpsPat = "https://" + hostUtf8 + "/*";
        const char* allowList[] = { httpPat.constData(), httpsPat.constData(), nullptr };
        WebKitUserStyleSheet* sheet = webkit_user_style_sheet_new(
            css.toUtf8().constData(),
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_STYLE_LEVEL_USER,
            allowList, nullptr);
        webkit_user_content_manager_add_style_sheet(ucm, sheet);
        webkit_user_style_sheet_unref(sheet);
        qInfo() << "[ADBLOCK] cosmetic sheet installed for" << host << "-" << ruleCount << "rules";
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
    // Cosmetic sheets are the only user style sheets we install.
    webkit_user_content_manager_remove_all_style_sheets(ucm);
    g_object_set_data(G_OBJECT(ucm), kInstalledHostsKey, nullptr);
}

void AdBlockEngine::applyCosmetics(WPEWebPage* page)
{
    if (!m_engine || !s_enabled || !page) return;

    // Hide CSS is installed pre-paint by installCosmetics(); only the
    // scriptlets (if any) remain a post-load JS injection.
    QByteArray urlUtf8 = page->url().toString().toUtf8();
    CosmeticResult cr = atlantic_adblock_get_cosmetic(m_engine, urlUtf8.constData());
    QString script;
    if (cr.injected_script && *cr.injected_script)
        script = QString::fromUtf8(cr.injected_script);
    atlantic_adblock_free_cosmetic(cr);

    if (!script.isEmpty())
        page->runJavaScript(script);
}

bool AdBlockEngine::isEnabled()
{
    return s_enabled;
}

void AdBlockEngine::setEnabled(bool enabled)
{
    s_enabled = enabled;
}
