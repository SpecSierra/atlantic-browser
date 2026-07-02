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

void AdBlockEngine::applyCosmetics(WPEWebPage* page)
{
    if (!m_engine || !s_enabled || !page) return;

    QByteArray urlUtf8 = page->url().toString().toUtf8();
    CosmeticResult cr = atlantic_adblock_get_cosmetic(m_engine, urlUtf8.constData());

    QStringList scripts;

    if (cr.hide_selectors && *cr.hide_selectors) {
        QString sel = QString::fromUtf8(cr.hide_selectors)
                          .replace('\\', "\\\\").replace('\'', "\\'");
        // Insert each selector as its OWN rule via insertRule() with try/catch.
        // The cosmetic list can contain a uBO procedural selector (e.g.
        // ".rpl-bottom-sheet:has-text(Get the app)") which is NOT valid CSS.
        // Joining every selector into one comma-separated group with a single
        // {display:none} means that one invalid selector makes the WHOLE group
        // rule invalid, and the CSS parser silently discards it — killing ALL
        // cosmetic hiding on every site (device-verified: the injected sheet had
        // 0 parsed rules on reddit). Per-rule insertion drops only the offending
        // selector and keeps the rest (50 of 51 survived on reddit → ad hidden).
        scripts << QStringLiteral(
            "(function(){var sels='%1'.split(',');"
            "var s=document.createElement('style');s.id='__atl_adblock_hide';"
            "document.documentElement.appendChild(s);var sh=s.sheet;"
            "for(var i=0;i<sels.length;i++){var t=sels[i].trim();if(!t)continue;"
            "try{sh.insertRule(t+'{display:none!important}',sh.cssRules.length);}catch(e){}}"
            "})()").arg(sel);
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

bool AdBlockEngine::isEnabled()
{
    return s_enabled;
}

void AdBlockEngine::setEnabled(bool enabled)
{
    s_enabled = enabled;
}
