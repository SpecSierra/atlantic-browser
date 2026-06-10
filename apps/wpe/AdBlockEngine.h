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
}

class WPEWebPage;

class AdBlockEngine {
public:
    static AdBlockEngine& instance();

    bool loadFromCache(const QString& path);
    bool isLoaded() const { return m_engine != nullptr; }

    void applyCosmetics(WPEWebPage* page);

    static bool isEnabled();
    static void setEnabled(bool enabled);

private:
    AdBlockEngine() = default;
    ~AdBlockEngine();
    AdBlockEngine(const AdBlockEngine&) = delete;

    AtlanticAdblockEngine* m_engine = nullptr;

    static bool s_enabled;
};
