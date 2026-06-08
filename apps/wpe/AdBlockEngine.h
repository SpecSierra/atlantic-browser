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

extern "C" {
    struct MatchResult { bool matched; bool important; char* redirect; char* exception; };
    struct CosmeticResult { const char* hide_selectors; const char* injected_script; const char* generated_css; };
    typedef void AtlanticAdblockEngine;

    AtlanticAdblockEngine* atlantic_adblock_create_from_cache(const uint8_t*, size_t);
    void                   atlantic_adblock_destroy(AtlanticAdblockEngine*);
    MatchResult            atlantic_adblock_match_network(AtlanticAdblockEngine*, const char* src, const char* req, const char* type, int third_party);
    void                   atlantic_adblock_free_match_result(MatchResult);
    CosmeticResult         atlantic_adblock_get_cosmetic(AtlanticAdblockEngine*, const char* url);
    void                   atlantic_adblock_free_cosmetic(CosmeticResult);
    void                   atlantic_adblock_enable_tag(AtlanticAdblockEngine*, const char*);
}

class WPEWebPage;

class AdBlockEngine {
public:
    static AdBlockEngine& instance();

    bool loadFromCache(const QString& path);
    bool isLoaded() const { return m_engine != nullptr; }

    bool shouldBlock(const QString& sourceUrl, const QString& requestUrl,
                     const char* resourceType, bool isThirdParty,
                     QString* redirectUrl = nullptr);

    void applyCosmetics(WPEWebPage* page);

private:
    AdBlockEngine() = default;
    ~AdBlockEngine();
    AdBlockEngine(const AdBlockEngine&) = delete;

    AtlanticAdblockEngine* m_engine = nullptr;
};
