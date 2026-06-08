/*
 * Copyright (c) 2013 - 2021 Jolla Ltd.
 * SPDX-License-Identifier: MPL-2.0
 */

#include "settingmanager.h"
#include "dbmanager.h"
#include "faviconmanager.h"
#include "../wpe/AdBlockEngine.h"

#include <MDConfItem>
#include <QVariant>

static SettingManager *gSingleton = 0;

SettingManager::SettingManager(QObject *parent)
    : QObject(parent)
    , m_searchEnginesInitialized(false)
    , m_addedSearchEngines(0)
{
    m_searchEngineConfItem = new MDConfItem("/apps/atlantic-browser/settings/search_engine", this);
    connect(m_searchEngineConfItem, &MDConfItem::valueChanged,
            this, &SettingManager::setSearchEngine);

    m_toolbarSmall = new MDConfItem("/apps/atlantic-browser/settings/toolbar_small", this);
    m_toolbarLarge = new MDConfItem("/apps/atlantic-browser/settings/toolbar_large", this);
    connect(m_toolbarSmall, &MDConfItem::valueChanged, this, &SettingManager::toolbarSmallChanged);
    connect(m_toolbarLarge, &MDConfItem::valueChanged, this, &SettingManager::toolbarLargeChanged);
}

int SettingManager::toolbarSmall()
{
    return m_toolbarSmall->value(72).value<int>();
}

int SettingManager::toolbarLarge()
{
    return m_toolbarLarge->value(108).value<int>();
}

SettingManager *SettingManager::instance()
{
    if (!gSingleton) {
        gSingleton = new SettingManager();
    }
    return gSingleton;
}

void SettingManager::clearHistory(int period)
{
    DBManager::instance()->clearHistory(period);
}

void SettingManager::clearCookiesAndSiteData()
{
    // WPE: cookies managed by WebKit internally; use webkit_website_data_manager if needed
}

void SettingManager::clearPasswords()
{
    FaviconManager::instance()->clear("logins");
}

void SettingManager::clearCache()
{
    // WPE: cache cleared via webkit_web_context_clear_cache
}

void SettingManager::clearSitePermissions()
{
    // WPE: permissions managed by WebKit internally
}

void SettingManager::removeAllTabs()
{
    DBManager::instance()->removeAllTabs();
}

void SettingManager::calculateCacheSize(QJSValue callback)
{
    if (!callback.isNull() && !callback.isUndefined() && callback.isCallable()) {
        callback.call(QJSValueList() << 0);
    }
}

void SettingManager::calculateSiteDataSize(QJSValue callback)
{
    if (!callback.isNull() && !callback.isUndefined() && callback.isCallable()) {
        callback.call(QJSValueList() << 0);
    }
}

void SettingManager::setSearchEngine()
{
    // WPE: search engine is configured in browser URL bar logic
}

void SettingManager::handleObserve(const QString &, const QVariant &)
{
    // WPE: no Gecko observer messages
}

bool SettingManager::isAdBlockEnabled() const
{
    return AdBlockEngine::isEnabled();
}

void SettingManager::setAdBlockEnabled(bool enabled)
{
    AdBlockEngine::setEnabled(enabled);
}
