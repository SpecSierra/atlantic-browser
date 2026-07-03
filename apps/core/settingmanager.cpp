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

#include <wpe/webkit.h>

// Engine export (webkit-wpe-dark-mode-runtime.patch): sets the global WebKit
// dark mode = what websites see as prefers-color-scheme, live pages included.
extern "C" void wpe_sfos_set_dark_mode(int darkMode);

namespace {
// Values match the Preferred color scheme combo in SettingsPage.qml
enum ColorScheme {
    PrefersLightMode = 0,
    PrefersDarkMode = 1,
    FollowsAmbience = 2
};
}

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

    m_colorScheme = new MDConfItem("/apps/atlantic-browser/settings/color_scheme", this);
    connect(m_colorScheme, &MDConfItem::valueChanged, this, &SettingManager::applyColorScheme);
    applyColorScheme();
}

void SettingManager::setAmbienceDark(bool dark)
{
    if (m_ambienceDark == dark)
        return;
    m_ambienceDark = dark;
    applyColorScheme();
}

void SettingManager::applyColorScheme()
{
    int scheme = m_colorScheme->value(FollowsAmbience).toInt();
    bool dark = scheme == PrefersDarkMode || (scheme == FollowsAmbience && m_ambienceDark);
    wpe_sfos_set_dark_mode(dark);
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
    WebKitWebsiteDataManager *manager =
        webkit_network_session_get_website_data_manager(webkit_network_session_get_default());
    // Everything except the HTTP caches (those are clearCache()'s job).
    WebKitWebsiteDataTypes types = static_cast<WebKitWebsiteDataTypes>(
        WEBKIT_WEBSITE_DATA_COOKIES
        | WEBKIT_WEBSITE_DATA_SESSION_STORAGE
        | WEBKIT_WEBSITE_DATA_LOCAL_STORAGE
        | WEBKIT_WEBSITE_DATA_INDEXEDDB_DATABASES
        | WEBKIT_WEBSITE_DATA_OFFLINE_APPLICATION_CACHE
        | WEBKIT_WEBSITE_DATA_HSTS_CACHE
        | WEBKIT_WEBSITE_DATA_ITP
        | WEBKIT_WEBSITE_DATA_SERVICE_WORKER_REGISTRATIONS
        | WEBKIT_WEBSITE_DATA_DOM_CACHE
        | WEBKIT_WEBSITE_DATA_DEVICE_ID_HASH_SALT);
    webkit_website_data_manager_clear(manager, types, 0, nullptr,
        [](GObject *object, GAsyncResult *result, gpointer) {
            GError *error = nullptr;
            webkit_website_data_manager_clear_finish(WEBKIT_WEBSITE_DATA_MANAGER(object), result, &error);
            if (error) {
                qWarning("clearCookiesAndSiteData failed: %s", error->message);
                g_error_free(error);
            }
        }, nullptr);
}

void SettingManager::clearPasswords()
{
    FaviconManager::instance()->clear("logins");
}

void SettingManager::clearCache()
{
    WebKitWebsiteDataManager *manager =
        webkit_network_session_get_website_data_manager(webkit_network_session_get_default());
    WebKitWebsiteDataTypes types = static_cast<WebKitWebsiteDataTypes>(
        WEBKIT_WEBSITE_DATA_MEMORY_CACHE | WEBKIT_WEBSITE_DATA_DISK_CACHE);
    webkit_website_data_manager_clear(manager, types, 0, nullptr,
        [](GObject *object, GAsyncResult *result, gpointer) {
            GError *error = nullptr;
            webkit_website_data_manager_clear_finish(WEBKIT_WEBSITE_DATA_MANAGER(object), result, &error);
            if (error) {
                qWarning("clearCache failed: %s", error->message);
                g_error_free(error);
            }
        }, nullptr);
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
