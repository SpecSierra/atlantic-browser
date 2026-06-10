.pragma library

/*
 * Shared address-bar input helpers: decide whether typed text is a URL or a
 * search query, and turn it into a loadable URL. Used by Overlay.qml and
 * browser-minimal.qml so the two stay in sync.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Heuristic: does the entered text look like a URL (vs. a search query)?
function isUrl(text) {
    text = text.trim();
    if (text.indexOf("://") !== -1) return true;
    if (text.indexOf("about:") === 0) return true;
    if (/^[a-zA-Z0-9\-]+\.[a-zA-Z]{2,}/.test(text)) return true;
    if (/^localhost(:[0-9]+)?/.test(text)) return true;
    if (/^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+/.test(text)) return true;
    return false;
}

// Turn entered text into a loadable URL: a search query becomes a Google
// search, a bare host gets an https:// scheme. Returns "" for empty input.
function normalize(text) {
    var candidate = text.trim();
    if (!candidate.length) return "";
    if (!isUrl(candidate)) {
        return "https://www.google.com/search?q=" + encodeURIComponent(candidate);
    }
    if (candidate.indexOf("://") === -1 && candidate.indexOf("about:") !== 0) {
        return "https://" + candidate;
    }
    return candidate;
}
