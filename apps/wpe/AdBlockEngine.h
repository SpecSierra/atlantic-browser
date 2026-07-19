/*
 * Copyright (c) 2026
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once
#include <QString>
#include <QStringList>
#include <QByteArray>

// The UI-process AdBlockEngine handles cosmetic filtering and popup
// (new-window navigation) blocking; per-request network blocking lives in the
// WebProcess adblock extension. Popups never reach the extension's
// send-request hook as a blockable subresource — they arrive in the UI
// process as a NEW_WINDOW policy decision — hence the network-match FFI here.
extern "C" {
    struct CosmeticResult { const char* hide_selectors; const char* injected_script; const char* generated_css; };
    struct MatchResult { bool matched; bool important; char* redirect; char* exception; };
    typedef void AtlanticAdblockEngine;

    AtlanticAdblockEngine* atlantic_adblock_create_from_cache(const uint8_t*, size_t);
    void                   atlantic_adblock_destroy(AtlanticAdblockEngine*);
    CosmeticResult         atlantic_adblock_get_cosmetic(AtlanticAdblockEngine*, const char* url);
    void                   atlantic_adblock_free_cosmetic(CosmeticResult);
    bool                   atlantic_adblock_use_resources_json(AtlanticAdblockEngine*, const uint8_t*, size_t);
    MatchResult            atlantic_adblock_match_network(AtlanticAdblockEngine*, const char* src_url,
                                                          const char* req_url, const char* type,
                                                          int third_party);
    void                   atlantic_adblock_free_match_result(MatchResult);
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

    // Should a popup / new-window navigation from pageUrl to popupUrl be
    // blocked? Matches popupUrl against the network filters as a top-level
    // document load (catches ad/redirect hosts on the list).
    bool shouldBlockPopup(const QUrl& pageUrl, const QUrl& popupUrl);

    static bool isEnabled();
    static void setEnabled(bool enabled);

    // Per-site allowlist: hosts (and their subdomains) on which all blocking —
    // network (extension side), cosmetics, generic hides, popups — is skipped.
    static void setAllowlist(const QStringList& hosts);
    static bool isAllowlistedUrl(const QUrl& url);
    // The list joined with '\n', for the WebProcess extension user message /
    // init user-data.
    static QByteArray allowlistJoined();

    // Same first-party relation as the WebProcess extension: equal hosts or
    // one a dotted suffix of the other.
    static bool areHostsRelated(const QString& a, const QString& b);

private:
    static QStringList s_allowlist;

    AdBlockEngine() = default;
    ~AdBlockEngine();
    AdBlockEngine(const AdBlockEngine&) = delete;

    AtlanticAdblockEngine* m_engine = nullptr;

    static bool s_enabled;
};
