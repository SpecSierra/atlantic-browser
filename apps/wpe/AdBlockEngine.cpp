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
#include <QStandardPaths>
#include <QTimer>

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

bool AdBlockEngine::shouldBlock(const QString& sourceUrl, const QString& requestUrl,
                                 const char* resourceType, bool isThirdParty,
                                 QString* redirectUrl)
{
    if (!m_engine) return false;

    QByteArray src = sourceUrl.toUtf8();
    QByteArray req = requestUrl.toUtf8();

    MatchResult r = atlantic_adblock_match_network(
        m_engine, src.constData(), req.constData(),
        resourceType, isThirdParty ? 1 : 0);

    bool blocked = r.matched && r.redirect == nullptr;
    if (r.redirect && redirectUrl) {
        *redirectUrl = QString::fromUtf8(r.redirect);
        blocked = true;
    }
    atlantic_adblock_free_match_result(r);
    return blocked;
}

void AdBlockEngine::applyCosmetics(WPEWebPage* page)
{
    if (!m_engine || !page) return;

    QByteArray urlUtf8 = page->url().toString().toUtf8();
    CosmeticResult cr = atlantic_adblock_get_cosmetic(m_engine, urlUtf8.constData());

    QStringList scripts;

    if (cr.hide_selectors && *cr.hide_selectors) {
        QString sel = QString::fromUtf8(cr.hide_selectors)
                          .replace('\\', "\\\\").replace('\'', "\\'");
        scripts << QStringLiteral(
            "(function(){var s=document.createElement('style');"
            "s.id='__atl_adblock_hide';"
            "s.textContent='%1{display:none!important}';"
            "document.documentElement.appendChild(s);})()").arg(sel);
    }

    if (cr.generated_css && *cr.generated_css) {
        QString css = QString::fromUtf8(cr.generated_css)
                          .replace('\\', "\\\\").replace('\'', "\\'")
                          .replace('\n', ' ');
        scripts << QStringLiteral(
            "(function(){var s=document.createElement('style');"
            "s.id='__atl_adblock_gen';"
            "s.textContent='%1';"
            "document.documentElement.appendChild(s);})()").arg(css);
    }

    if (cr.injected_script && *cr.injected_script) {
        scripts << QString::fromUtf8(cr.injected_script);
    }

    atlantic_adblock_free_cosmetic(cr);

    for (const QString& s : scripts) {
        page->runJavaScript(s);
    }
}
