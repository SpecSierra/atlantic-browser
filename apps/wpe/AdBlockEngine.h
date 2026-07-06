/*
 * Copyright (c) 2026
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once
#include <QString>
#include <QByteArray>

// The UI-process AdBlockEngine handles cosmetic filtering only; network
// blocking lives in the WebProcess adblock extension. Network-match FFI lives
// there, not here.
extern "C" {
    struct CosmeticResult { const char* hide_selectors; const char* injected_script; const char* generated_css; };
    typedef void AtlanticAdblockEngine;

    AtlanticAdblockEngine* atlantic_adblock_create_from_cache(const uint8_t*, size_t);
    void                   atlantic_adblock_destroy(AtlanticAdblockEngine*);
    CosmeticResult         atlantic_adblock_get_cosmetic(AtlanticAdblockEngine*, const char* url);
    void                   atlantic_adblock_free_cosmetic(CosmeticResult);
    bool                   atlantic_adblock_use_resources_json(AtlanticAdblockEngine*, const uint8_t*, size_t);
    char*                  atlantic_adblock_get_generic_hides(AtlanticAdblockEngine*, const char* url,
                                                              const char* classes, const char* ids);
    void                   atlantic_adblock_free_string(char*);
}

class WPEWebPage;
class QUrl;
typedef struct _WebKitUserContentManager WebKitUserContentManager;

class AdBlockEngine {
public:
    static AdBlockEngine& instance();

    bool loadFromCache(const QString& path);
    // uBO scriptlet/redirect resources (Brave adblock-resources JSON); not part
    // of the engine cache, load after loadFromCache. Enables ##+js(...) rules.
    bool loadResources(const QString& path);
    bool isLoaded() const { return m_engine != nullptr; }

    // Pre-paint cosmetic hiding: install the hide selectors for url's host as
    // a document-start user style sheet on the view's content manager (once
    // per host per manager). Call at load-committed so the CSS is present
    // before the document first paints — no ad flash / layout shift.
    void installCosmetics(WebKitUserContentManager* ucm, const QUrl& url);
    // Remove all installed cosmetic sheets (adblock toggled off).
    static void resetCosmetics(WebKitUserContentManager* ucm);

    // Generic cosmetic filtering: given the class/id names present in the DOM
    // (newline-separated), return the newline-separated generic hide selectors
    // that apply on url (site exceptions honoured). Empty result = nothing new.
    QString genericHides(const QUrl& url, const QByteArray& classes, const QByteArray& ids);

    static bool isEnabled();
    static void setEnabled(bool enabled);

private:
    AdBlockEngine() = default;
    ~AdBlockEngine();
    AdBlockEngine(const AdBlockEngine&) = delete;

    AtlanticAdblockEngine* m_engine = nullptr;

    static bool s_enabled;
};
