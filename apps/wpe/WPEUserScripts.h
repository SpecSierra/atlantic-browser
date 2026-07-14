#pragma once

// JavaScript user-scripts injected into web pages by WPEWebPage.
// Extracted verbatim from WPEWebPage.cpp to keep that file manageable;
// these are the identical compiled-in raw string literals (no behaviour
// change). Edit JS here; it is referenced by name from WPEWebPage.cpp.

namespace WPEUserScripts {

static const char* const kSelectionBridge = R"JS(
(function() {
    if (window.__wpeSelectionBridgeInstalled) return;
    window.__wpeSelectionBridgeInstalled = true;

    function caretRect(node, offset) {
        if (!node)
            return null;
        var range = document.createRange();
        try {
            range.setStart(node, offset);
            range.setEnd(node, offset);
        } catch (e) {
            return null;
        }
        var rect = range.getBoundingClientRect();
        if (!rect)
            return null;
        return rect;
    }

    function pointRange(x, y) {
        if (document.caretRangeFromPoint)
            return document.caretRangeFromPoint(x, y);
        if (document.caretPositionFromPoint) {
            var p = document.caretPositionFromPoint(x, y);
            if (p) {
                var r = document.createRange();
                r.setStart(p.offsetNode, p.offset);
                r.collapse(true);
                return r;
            }
        }
        return null;
    }

    function selectWordAtPoint(x, y) {
        var sel = window.getSelection ? window.getSelection() : null;
        if (!sel)
            return false;

        var range = pointRange(x, y);
        if (!range)
            return false;

        var node = range.startContainer;
        var offset = range.startOffset;
        if (!node)
            return false;

        if (node.nodeType !== Node.TEXT_NODE) {
            if (node.childNodes && node.childNodes.length) {
                if (offset >= node.childNodes.length)
                    offset = node.childNodes.length - 1;
                if (offset < 0)
                    offset = 0;
                var child = node.childNodes[offset];
                if (child && child.nodeType === Node.TEXT_NODE) {
                    node = child;
                    offset = 0;
                }
            }
        }

        if (node.nodeType !== Node.TEXT_NODE)
            return false;

        var text = node.data || '';
        if (!text.length)
            return false;

        var start = offset;
        var end = offset;
        while (start > 0 && /\S/.test(text.charAt(start - 1)))
            start--;
        while (end < text.length && /\S/.test(text.charAt(end)))
            end++;

        if (start === end) {
            if (end < text.length)
                end++;
            else if (start > 0)
                start--;
        }

        try {
            var wordRange = document.createRange();
            wordRange.setStart(node, start);
            wordRange.setEnd(node, end);
            sel.removeAllRanges();
            sel.addRange(wordRange);
            return true;
        } catch (e) {
            return false;
        }
    }

    function selectionPayload() {
        var sel = window.getSelection ? window.getSelection() : null;
        if (!sel || !sel.rangeCount || sel.isCollapsed)
            return { type: 'clear' };

        var text = sel.toString();
        if (!text)
            return { type: 'clear' };

        var range = sel.getRangeAt(0);
        // Use range.start/end (order-invariant) instead of anchor/focus (drag-direction dependent).
        // Avoid getClientRects() — it forces synchronous layout on every selectionchange, which
        // is very expensive on heavy-DOM pages. Caret rects on collapsed ranges are much cheaper.
        var startRect = caretRect(range.startContainer, range.startOffset);
        var endRect = caretRect(range.endContainer, range.endOffset);
        var bounds = (!startRect || !endRect) ? range.getBoundingClientRect() : null;

        var sx = startRect ? startRect.left : (bounds ? bounds.left : 0);
        var sy = startRect ? startRect.bottom : (bounds ? bounds.bottom : 0);
        var ex = endRect ? endRect.right : (bounds ? bounds.right : 0);
        var ey = endRect ? endRect.bottom : (bounds ? bounds.bottom : 0);

        return {
            type: 'select',
            text: text,
            startX: sx,
            startY: sy,
            endX: ex,
            endY: ey,
            cursorX: ex,
            cursorY: ey,
            sx: sx,
            sy: sy,
            ex: ex,
            ey: ey
        };
    }

    var pendingSelectionRaf = null;

    function flushSelection() {
        try {
            window.webkit.messageHandlers.selectionBridge.postMessage(selectionPayload());
        } catch (ex) {
            console.error('[WPE-SELECT-JS] postMessage error: ' + ex);
        }
    }

    // Throttle continuous selectionchange events to one IPC message per animation frame.
    function postSelection() {
        if (pendingSelectionRaf) return;
        pendingSelectionRaf = requestAnimationFrame(function() {
            pendingSelectionRaf = null;
            flushSelection();
        });
    }

    // For gesture-end events, cancel any pending frame and send immediately so the
    // final handle positions are never one frame late.
    function postSelectionFinal() {
        if (pendingSelectionRaf) {
            cancelAnimationFrame(pendingSelectionRaf);
            pendingSelectionRaf = null;
        }
        flushSelection();
    }

    var longPressTimer = null;
    var longPressPoint = null;
    var longPressStartPoint = null;
    var longPressMoveThreshold = 12;

    function cancelLongPress() {
        if (longPressTimer) {
            clearTimeout(longPressTimer);
            longPressTimer = null;
        }
        longPressPoint = null;
        longPressStartPoint = null;
    }

    function imageUrlAtPoint(x, y) {
        var el = document.elementFromPoint(x, y);
        while (el) {
            if (el.tagName && el.tagName.toLowerCase() === 'img') {
                var src = el.currentSrc || el.src || el.getAttribute('src');
                if (src) return src;
            }
            el = el.parentElement;
        }
        return null;
    }

    // Returns true if an image was found at (x, y) and its URL handed to the
    // QML image panel. Shared by both long-press triggers below.
    function postImageLongPress(x, y) {
        var imgUrl = imageUrlAtPoint(x, y);
        if (!imgUrl)
            return false;
        try {
            window.webkit.messageHandlers.imageLongPressBridge.postMessage(
                { imageUrl: imgUrl, x: x, y: y });
        } catch(e) {}
        return true;
    }

    function beginLongPress(x, y) {
        cancelLongPress();
        longPressPoint = { x: x, y: y };
        longPressStartPoint = { x: x, y: y };
        longPressTimer = setTimeout(function() {
            longPressTimer = null;
            if (!longPressPoint) return;
            var lx = longPressPoint.x, ly = longPressPoint.y;
            longPressPoint = null;
            if (postImageLongPress(lx, ly))
                return;
            if (selectWordAtPoint(lx, ly))
                postSelectionFinal();
        }, 350);
    }

    function touchPointFromEvent(e) {
        if (e.touches && e.touches.length)
            return e.touches[0];
        if (e.changedTouches && e.changedTouches.length)
            return e.changedTouches[0];
        return null;
    }

    document.addEventListener('selectionchange', postSelection, true);
    document.addEventListener('mouseup', postSelectionFinal, true);
    document.addEventListener('touchend', postSelectionFinal, {capture: true, passive: true});
    document.addEventListener('keyup', postSelectionFinal, true);
    document.addEventListener('contextmenu', function(e) {
        // WebKit's native long-press fires contextmenu reliably (this is the
        // working text-selection trigger); the JS touch-timer above can be
        // pre-empted by the compositor's scroll/gesture handling. Check for an
        // image here too so the image panel uses the same dependable signal.
        cancelLongPress();
        if (postImageLongPress(e.clientX, e.clientY)) {
            e.preventDefault();
            return;
        }
        if (selectWordAtPoint(e.clientX, e.clientY)) {
            e.preventDefault();
            postSelectionFinal();
        }
    }, true);
    // passive:true lets WebKit compositor scroll without waiting for main-thread
    // confirmation that preventDefault() won't be called (none of these handlers call it)
    document.addEventListener('touchstart', function(e) {
        var p = touchPointFromEvent(e);
        if (p)
            beginLongPress(p.clientX, p.clientY);
    }, {capture: true, passive: true});
    document.addEventListener('touchmove', function(e) {
        var p = touchPointFromEvent(e);
        if (!p || !longPressStartPoint)
            return;
        var dx = p.clientX - longPressStartPoint.x;
        var dy = p.clientY - longPressStartPoint.y;
        if ((dx * dx) + (dy * dy) > longPressMoveThreshold * longPressMoveThreshold)
            cancelLongPress();
    }, {capture: true, passive: true});
    document.addEventListener('touchcancel', cancelLongPress, {capture: true, passive: true});
    document.addEventListener('touchend', cancelLongPress, {capture: true, passive: true});
})();
)JS";

// Cross-origin iframes are invisible to the main-frame editable probe that
// drives the virtual keyboard (contentDocument access throws), so inputs in
// e.g. embedded payment/comment widgets never raised the keyboard. This
// bridge runs inside every cross-origin subframe and reports editable
// focus/blur to the UI process, which shows the keyboard and keeps the
// probe from hiding it while the subframe field stays focused.
static const char* const kEditableFocusBridge = R"JS(
(function() {
    if (window === window.top) return;
    try { void window.top.document; return; } catch (e) {}
    if (window.__wpeEditableFocusBridgeInstalled) return;
    window.__wpeEditableFocusBridgeInstalled = true;
    function isEditable(e) {
        if (!e) return false;
        if (e.isContentEditable) return true;
        var tag = (e.tagName || '').toLowerCase();
        if (tag === 'textarea') return !e.readOnly && !e.disabled;
        if (tag === 'input') {
            var t = (e.type || 'text').toLowerCase();
            var blocked = {button:1,submit:1,reset:1,checkbox:1,radio:1,file:1,image:1,range:1,color:1,hidden:1};
            return !blocked[t] && !e.readOnly && !e.disabled;
        }
        return false;
    }
    var focused = false;
    function post(v) {
        if (focused === v) return;
        focused = v;
        try { window.webkit.messageHandlers.editableFocus.postMessage(v ? 1 : 0); } catch (e) {}
    }
    document.addEventListener('focusin', function(ev) {
        if (isEditable(ev.target)) post(true);
    }, true);
    document.addEventListener('focusout', function(ev) {
        if (isEditable(ev.target)) post(false);
    }, true);
    window.addEventListener('pagehide', function() { post(false); }, true);
})();
)JS";

static const char* const kPerfCss = R"JS(
(function() {
    if (document.getElementById('__wpe_perf_style')) return;
    var s = document.createElement('style');
    s.id = '__wpe_perf_style';
    s.textContent = '* { backdrop-filter: none !important; -webkit-backdrop-filter: none !important;'
                  + ' -webkit-tap-highlight-color: rgba(0,0,0,0) !important; }';
    (document.head || document.documentElement).appendChild(s);
})();
)JS";

// YouTube player control icons come up blank on WPE 2.52. There are TWO distinct
// players and TWO distinct icon mechanisms — this fixes both:
//
// 1. Legacy / large play button: inline <svg><path class="ytp-svg-fill">. Its
//    computed fill resolves to black (YouTube's intended white never applies),
//    invisible on the dark player. A single `.ytp-svg-fill{fill:#fff}` rule
//    flips it white. Cheap and harmless everywhere, so it stays global.
//
// 2. Mobile player (m.youtube.com, which our Safari-shaped UA now gets) renders
//    its controls with YouTube's modern <c3-icon> web component:
//      <c3-icon><span class="yt-icon-shape"><div style="fill:currentcolor">
//        <svg viewBox="0 0 24 24"><path d="…"></svg></div></span></c3-icon>
//    WPE fails to PAINT these inline player <svg> glyphs — fullscreen, seek
//    fwd/back, mute and prev/next are blank — even though the identically
//    structured settings gear and the page's action icons (like/share/save)
//    DO paint. So it is not a colour/fill problem (device-verified: the broken
//    and the working buttons have byte-identical computed fill, both white).
//    WPE does, however, reliably paint a CSS -webkit-mask-image fed an inline
//    data: URI (the same path the generic icon healer relies on). So for every
//    such glyph, re-issue it as a mask sourced from YouTube's OWN <svg> with an
//    explicit white fill, and hide the non-painting original. This reuses the
//    real glyphs (language-agnostic, nothing hand-drawn, robust across player
//    redesigns) — the proven replacement for the old per-icon aria-label hack.
//    Controls are torn down/rebuilt on every show/hide, so a MutationObserver +
//    periodic re-scan keeps them patched. Scoped to the player control overlays
//    (the mobile bar lives in <ytm-watch-player-controls>, a SIBLING of
//    #movie_player — not inside it) on youtube hosts, so the page's own
//    (already-fine) action icons and other sites are untouched.
static const char* const kYouTubeIconFix = R"JS(
(function() {
    if (!document.getElementById('__wpe_yt_icon_fix')) {
        var s = document.createElement('style');
        s.id = '__wpe_yt_icon_fix';
        s.textContent = '.ytp-svg-fill{fill:#fff !important;}';
        (document.head || document.documentElement).appendChild(s);
    }

    if (!/(^|\.)youtube\.com$/.test(location.hostname)) return;
    if (window.__wpeYtPlayerIconFix) return;
    window.__wpeYtPlayerIconFix = true;

    // Re-issue the (non-painting) inline <svg> in `host` as a CSS mask + white
    // fill on the icon-sized box. `size` is the mask-size: 'contain' fills the
    // box (icon hosts whose box IS the glyph), or an explicit px for a glyph
    // that sits in a larger padded box (e.g. the unmute popup's 50x49 area).
    //
    // YouTube mutates a button's glyph IN PLACE (fullscreen<->exit, play<->pause,
    // mute<->unmute, autoplay on<->off) by swapping the <svg>'s contents on the
    // SAME host node. A one-shot boolean cache would leave a stale/empty mask —
    // e.g. the fullscreen icon goes blank after a fullscreen round-trip. So key
    // the cache on the glyph's CONTENT signature and re-derive whenever it
    // changes; and never cache while the <svg> has no drawable shape yet (the
    // icon often lazy-loads its <path> a frame after the node appears — masking
    // then would bake in an empty, fully-transparent mask = invisible).
    function maskHost(host, size, boxOverride) {
        var svg = host.querySelector('svg');
        if (!svg) return;
        var sig = svg.innerHTML;
        if (!/<(path|polygon|rect|circle|ellipse|use)/i.test(sig)) return;  // no glyph yet
        if (host.__wpeMaskSig === sig) return;                              // already current
        var clone = svg.cloneNode(true);
        clone.setAttribute('fill', '#000');                 // solid mask alpha
        var glyphs = clone.querySelectorAll('path,polygon,rect,circle,ellipse');
        for (var i = 0; i < glyphs.length; i++) glyphs[i].setAttribute('fill', '#000');
        var uri = 'url("data:image/svg+xml,'
                + encodeURIComponent(new XMLSerializer().serializeToString(clone)) + '")';
        // The element we paint the mask onto must be one WPE actually composites.
        // Default: the host's own icon-sized box. Some hosts (the Shorts action
        // bar) sit on a leaf the compositor never paints, so the caller passes an
        // ancestor box that DOES paint (see the reel-action-bar scan below).
        var box = boxOverride || host.querySelector('div') || host;
        size = size || 'contain';
        box.style.setProperty('-webkit-mask-image', uri, 'important');
        box.style.setProperty('mask-image', uri, 'important');
        box.style.setProperty('-webkit-mask-size', size, 'important');
        box.style.setProperty('mask-size', size, 'important');
        box.style.setProperty('-webkit-mask-repeat', 'no-repeat', 'important');
        box.style.setProperty('mask-repeat', 'no-repeat', 'important');
        box.style.setProperty('-webkit-mask-position', 'center', 'important');
        box.style.setProperty('mask-position', 'center', 'important');
        box.style.setProperty('background-color', '#fff', 'important');
        svg.style.setProperty('visibility', 'hidden', 'important');
        host.__wpeMaskSig = sig;
    }

    // The mobile player's control bar is NOT inside #movie_player — it lives in
    // a sibling overlay (<ytm-watch-player-controls>, under .player-container).
    // Device-confirmed: #movie_player holds 0 of these icon hosts; the 8 control
    // glyphs are under ytm-watch-player-controls. Scope to the control overlays
    // (plus #movie_player / #player as fallbacks for other variants) so we patch
    // ONLY player chrome, never the page's own (already-fine) action icons.
    // The "Tap to unmute" popup is a separate ytp overlay using the legacy
    // .ytp-svg-fill inline <svg> (no .yt-icon-shape) and hits the same paint
    // failure — mask it too, at the glyph's native size (its box is padded).
    function scan() {
        var hosts = document.querySelectorAll(
            'ytm-watch-player-controls .yt-icon-shape, '
          + '.player-controls-bottom .yt-icon-shape, '
          + '.player-controls-middle .yt-icon-shape, '
          + '.player-controls-top .yt-icon-shape, '
          + '#movie_player .yt-icon-shape, #player .yt-icon-shape');
        for (var i = 0; i < hosts.length; i++) maskHost(hosts[i]);
        // YouTube Shorts right-side action bar (like / dislike / comments / share)
        // lives in <reel-action-bar-view-model>, outside the watch-player overlay,
        // so the scan above never reaches it. Extra twist, device-proven with a
        // per-layer paint test: WPE does NOT composite the <c3-icon>/.yt-icon-shape
        // leaf here (a solid background on it never reaches the screen), but it DOES
        // composite the wrapping div.ytSpecButtonShapeNextIcon. So mask THAT ancestor
        // with the leaf's glyph — masking the leaf itself paints nothing on screen.
        var reel = document.querySelectorAll('reel-action-bar-view-model .yt-icon-shape');
        for (var r = 0; r < reel.length; r++) {
            var box = reel[r].closest('div.ytSpecButtonShapeNextIcon')
                   || reel[r].closest('div');
            if (box) maskHost(reel[r], 'contain', box);
        }
        var unmute = document.querySelectorAll('.ytp-unmute-icon');
        for (var j = 0; j < unmute.length; j++) {
            var u = unmute[j], usvg = u.querySelector('svg');
            maskHost(u, usvg ? (usvg.getAttribute('width') || '24') + 'px' : '24px');
        }
        syncUnmute();
    }

    // YouTube's mobile player never surfaces the legacy "Tap to unmute" popup on
    // WPE — it stays display:none even during muted autoplay (device-confirmed:
    // muted+playing, .ytp-unmute is display:none / 0x0), though the tap-to-unmute
    // hit area still works, so the affordance is simply invisible. Force the
    // popup visible whenever the video is muted (its own CSS positions it
    // top-left; its icon is masked above); drop the override once unmuted so it
    // returns to YouTube's hidden state. Only touches display/opacity — never the
    // video's mute state or audio.
    function syncUnmute() {
        var u = document.querySelector('.ytp-unmute');
        if (!u) return;
        var v = document.querySelector('video');
        if (v && v.muted) {
            u.style.setProperty('display', 'block', 'important');
            u.style.setProperty('opacity', '1', 'important');
            u.style.setProperty('visibility', 'visible', 'important');
        } else {
            // YouTube hides this popup via an inline display:none which our
            // override clobbered; re-assert none (not removeProperty, which would
            // fall back to the stylesheet's default display:block = stuck open).
            u.style.setProperty('display', 'none', 'important');
            u.style.removeProperty('opacity');
            u.style.removeProperty('visibility');
        }
    }

    var pending = false;
    function schedule() {
        if (pending) return;
        pending = true;
        var run = function() { pending = false; scan(); };
        if (window.requestAnimationFrame) requestAnimationFrame(run);
        else setTimeout(run, 16);
    }

    var mo = new MutationObserver(schedule);
    mo.observe(document.documentElement, { childList: true, subtree: true });
    setInterval(scan, 800);   // safety net for mutations the observer coalesces away
    // mute/unmute doesn't mutate the DOM, so react to it directly (volumechange
    // doesn't bubble — capture phase catches it) to toggle the unmute popup.
    document.addEventListener('volumechange', schedule, true);
    scan();
})();
)JS";

static const char* const kIconHeal = R"JS(
(function() {
    if (window.__wpeIconHealInstalled) return;
    window.__wpeIconHealInstalled = true;

    var inlinePromises = {};   // absolute url -> Promise<data URI>
    var seenSheets = [];       // stylesheets already walked
    var maskSelectors = [];    // selectors of url()-mask rules
    var healedCount = 0;
    var styleEl = null;

    function overrideSheet() {
        if (!styleEl || !styleEl.parentNode) {
            styleEl = document.createElement('style');
            styleEl.id = '__wpe_icon_heal';
            (document.head || document.documentElement).appendChild(styleEl);
        }
        return styleEl.sheet;
    }

    function inlineUrl(abs) {
        if (!inlinePromises[abs]) {
            inlinePromises[abs] = fetch(abs, { credentials: 'omit' })
                .then(function(r) {
                    if (!r.ok) throw new Error('http ' + r.status);
                    return r.blob();
                })
                .then(function(blob) {
                    return new Promise(function(resolve, reject) {
                        var fr = new FileReader();
                        fr.onload = function() { resolve(fr.result); };
                        fr.onerror = reject;
                        fr.readAsDataURL(blob);
                    });
                });
        }
        return inlinePromises[abs];
    }

    function maskValueOf(style) {
        return style.getPropertyValue('-webkit-mask-image')
            || style.getPropertyValue('mask-image')
            || style.getPropertyValue('-webkit-mask')
            || style.getPropertyValue('mask')
            || '';
    }

    function externalUrlIn(value, baseHref) {
        if (value.indexOf('url(') === -1) return null;
        if (value.indexOf('data:') !== -1 || value.indexOf('gradient') !== -1) return null;
        var m = value.match(/url\((["']?)([^)"']+)\1\)/);
        if (!m) return null;
        var abs;
        try { abs = new URL(m[2], baseHref || document.baseURI).href; } catch (e) { return null; }
        return abs.indexOf('http') === 0 ? abs : null;
    }

    function walkRules(rules, baseHref) {
        for (var i = 0; i < rules.length; i++) {
            var rule = rules[i];
            if (rule.styleSheet) { walkSheet(rule.styleSheet); continue; }      // @import
            if (rule.cssRules) { walkRules(rule.cssRules, baseHref); continue; } // @media etc.
            if (!rule.style || !rule.selectorText) continue;
            var mv = maskValueOf(rule.style);
            if (!mv || mv.indexOf('url(') === -1 || mv.indexOf('gradient') !== -1) continue;
            var sel = rule.selectorText;
            if (maskSelectors.indexOf(sel) === -1) maskSelectors.push(sel);
            var abs = externalUrlIn(mv, baseHref);
            if (abs) {
                (function(sel2, abs2) {
                    inlineUrl(abs2).then(function(dataUri) {
                        var sheet = overrideSheet();
                        sheet.insertRule(sel2 + '{-webkit-mask-image:url("' + dataUri + '") !important;'
                                              + 'mask-image:url("' + dataUri + '") !important;}',
                                         sheet.cssRules.length);
                        healedCount++;
                        console.log('[WPE-ICON-HEAL] inlined external mask ' + abs2.slice(0, 120));
                    }).catch(function(e) {
                        console.log('[WPE-ICON-HEAL] cannot inline ' + abs2.slice(0, 120) + ': ' + e);
                    });
                })(sel, abs);
            }
            // Pseudo-elements cannot get inline styles; if a masked pseudo
            // declares no background paint at all, give it currentColor.
            if (/::?(before|after)/.test(sel)
                && !rule.style.getPropertyValue('background-color')
                && !rule.style.getPropertyValue('background')
                && !rule.style.getPropertyValue('background-image')) {
                try {
                    var s2 = overrideSheet();
                    s2.insertRule(sel + '{background-color:currentColor !important;}', s2.cssRules.length);
                } catch (e) {}
            }
        }
    }

    function walkSheet(sheet) {
        if (!sheet || seenSheets.indexOf(sheet) !== -1) return;
        seenSheets.push(sheet);
        var rules = null;
        try { rules = sheet.cssRules; } catch (e) { return; } // cross-origin
        if (rules) walkRules(rules, sheet.href);
    }

    function elementPass() {
        for (var i = 0; i < maskSelectors.length; i++) {
            var nodes;
            try { nodes = document.querySelectorAll(maskSelectors[i]); } catch (e) { continue; }
            for (var j = 0; j < nodes.length; j++) {
                var el = nodes[j];
                if (el.__wpeIconHealed || el.childElementCount > 0) continue;
                if (el.textContent && el.textContent.trim()) continue;
                var cs = getComputedStyle(el);
                var mi = cs.webkitMaskImage || cs.maskImage || 'none';
                if (mi.indexOf('url(') === -1) continue;
                if (cs.backgroundImage !== 'none') continue;
                var bg = cs.backgroundColor;
                if (bg !== 'transparent' && !/rgba\([^)]+,\s*0\)$/.test(bg)) continue;
                var rect = el.getBoundingClientRect();
                if (!rect.width || !rect.height || rect.width > 160 || rect.height > 160) continue;
                el.style.setProperty('background-color', 'currentColor', 'important');
                el.__wpeIconHealed = true;
                healedCount++;
            }
        }
        if (healedCount && healedCount !== window.__wpeIconHealLogged) {
            window.__wpeIconHealLogged = healedCount;
            console.log('[WPE-ICON-HEAL] healed ' + healedCount + ' icon(s) so far');
        }
    }

    var passPending = false;
    function pass() {
        if (passPending) return;
        passPending = true;
        var run = function() {
            passPending = false;
            try {
                var sheets = document.styleSheets;
                for (var i = 0; i < sheets.length; i++) walkSheet(sheets[i]);
                elementPass();
            } catch (e) {}
        };
        if (window.requestIdleCallback) requestIdleCallback(run, { timeout: 1000 });
        else setTimeout(run, 50);
    }

    document.addEventListener('DOMContentLoaded', pass, true);
    window.addEventListener('load', function() {
        pass();
        setTimeout(pass, 1500);
        setTimeout(pass, 5000);
    });
    // Late-loading <link rel=stylesheet> (capture phase sees their load events).
    document.addEventListener('load', function(e) {
        if (e.target && e.target.tagName === 'LINK') pass();
    }, true);
    // Player chrome / lazy UI appears on fullscreen or interaction.
    document.addEventListener('fullscreenchange', pass, true);
    document.addEventListener('webkitfullscreenchange', pass, true);
    var lastInteractionPass = 0;
    var downX = 0, downY = 0;
    document.addEventListener('pointerdown', function(e) {
        downX = e.clientX; downY = e.clientY;
    }, { capture: true, passive: true });
    document.addEventListener('pointerup', function(e) {
        // Only heal on a genuine tap, never on a scroll/flick. A drag moves the
        // pointer; healing walks every stylesheet + querySelectorAll(maskSelectors)
        // + getComputedStyle (~145ms on heavy pages like franceinfo). Firing it on
        // every swipe-end (~1/sec) is a measured contributor to the scroll freeze -
        // the main thread was 92% busy during scroll, this being ~25% of it. Icons
        // that appear on interaction (player chrome) still heal on a real tap.
        if (Math.abs(e.clientX - downX) > 10 || Math.abs(e.clientY - downY) > 10) return;
        var now = Date.now();
        if (now - lastInteractionPass < 1000 || !maskSelectors.length) return;
        lastInteractionPass = now;
        pass();
    }, { capture: true, passive: true });
    pass();
})();
)JS";

static const char* const kPassiveScroll = R"JS(
(function() {
    try {
        var proto = EventTarget.prototype;
        if (proto.__wpePassivePatched) return;
        proto.__wpePassivePatched = true;
        var BLOCKERS = { touchstart: 1, touchmove: 1, wheel: 1, mousewheel: 1 };
        var orig = proto.addEventListener;
        proto.addEventListener = function(type, listener, options) {
            if (BLOCKERS[type] &&
                (this === window || this === document ||
                 this === document.documentElement || this === document.body)) {
                if (options === undefined || options === null || typeof options === 'boolean')
                    options = { capture: options === true, passive: true };
                else if (typeof options === 'object' && options.passive === undefined)
                    options = Object.assign({}, options, { passive: true });
            }
            return orig.call(this, type, listener, options);
        };
    } catch (e) {}
})();
)JS";

// Reddit feed video autoplay is the main remaining per-frame cost on this
// device. media_playback_requires_user_gesture (set globally in WPEWebPage)
// only blocks AUDIBLE autoplay; Reddit's feed posts are MUTED <video> elements
// that Reddit plays on scroll-into-view via an IntersectionObserver, which is
// permitted. On the libhybris/Adreno 610 a continuously-decoding muted video
// (often several queued as you scroll) competes with the compositor for the GPU
// and keeps a GStreamer pipeline + buffered media resident — visible as both
// "videos still autoplay" and lower scroll fps. Block it: pause any muted
// <video> that starts playing WITHOUT a transient user activation, and force
// preload=none so a gated element does not eagerly buffer (one paused feed video
// was observed buffering a full ~53 s HLS stream). A real tap-to-play sets
// navigator.userActivation.isActive for the gesture, so intentional playback is
// untouched; a scroll grants no activation (verified on device), so autoplay is.
//
// NOTE: an earlier version of this script applied content-visibility:auto to
// feed posts to bound memory. It was REMOVED: device A/B on build 331 showed it
// HALVED scroll fps (2.5 vs 5.8) because every post re-rendered as it re-entered
// the viewport, pegging the already GPU-bound compositor. The OOM/memory side is
// now handled in the engine by webkit-memory-pressure-threshold-env.patch (WebKit
// purges its caches at a realistic device budget), which bounds RSS without the
// per-scroll re-raster churn. Set ATLANTIC_DISABLE_REDDIT_PERF=1 to turn off.
static const char* const kRedditPerf = R"JS(
(function() {
    try {
        if (!/(^|\.)reddit\.com$/i.test(location.hostname)) return;
        if (window.__wpeRedditPerfInstalled) return;
        window.__wpeRedditPerfInstalled = true;

        function userInitiated() {
            return !!(navigator.userActivation && navigator.userActivation.isActive);
        }
        // A muted video starting to play with no transient user activation is
        // Reddit's scroll-into-view autoplay; pause it. Tap-to-play keeps the
        // activation, so it is allowed through.
        function tame(v) {
            if (!v || v.tagName !== 'VIDEO') return;
            try { if (v.preload !== 'none') v.preload = 'none'; } catch (e) {}
            try { v.autoplay = false; v.removeAttribute('autoplay'); } catch (e) {}
            if (!v.paused && v.muted && !userInitiated()) {
                try { v.pause(); } catch (e) {}
            }
        }
        // PRIMARY blocker: override play() itself. The 'play'/'playing' events
        // are NOT composed, so a document-level capture listener never sees plays
        // from inside shreddit-player's shadow DOM — which is exactly where
        // Reddit's feed videos live (device-verified: the capture listener saw 0
        // of the shadow-DOM autoplays while a muted video played). Overriding the
        // prototype method catches every play() call regardless of shadow
        // boundary. A muted video calling play() with no transient user activation
        // is scroll-into-view autoplay → reject like the browser's own autoplay
        // policy (NotAllowedError) so Reddit's player falls back to tap-to-play.
        var origPlay = HTMLMediaElement.prototype.play;
        HTMLMediaElement.prototype.play = function() {
            try {
                if (this.tagName === 'VIDEO' && this.muted && !userInitiated()) {
                    try { this.pause(); } catch (e) {}
                    return Promise.reject(
                        new DOMException('autoplay blocked by Atlantic', 'NotAllowedError'));
                }
            } catch (e) {}
            return origPlay.apply(this, arguments);
        };
        // Backstop for any light-DOM videos (these events don't cross shadow
        // boundaries, so the override above is what covers shreddit-player).
        document.addEventListener('play', function(e) { tame(e.target); }, true);
        document.addEventListener('playing', function(e) { tame(e.target); }, true);
        // Pause anything already autoplaying at install time.
        function sweep(root) {
            if (!root || !root.querySelectorAll) return;
            var vids = root.querySelectorAll('video');
            for (var i = 0; i < vids.length; i++) tame(vids[i]);
            var all = root.querySelectorAll('*');
            for (var j = 0; j < all.length; j++)
                if (all[j].shadowRoot) sweep(all[j].shadowRoot);
        }
        sweep(document);
        document.addEventListener('DOMContentLoaded', function() { sweep(document); }, { once: true });

        // Kill CSS blur. Reddit stacks blur(24px)-filtered copies of post media
        // as decorative ambient backdrops (8x 1080x1080 device-px imgs on a
        // single post page). WPE runs Gaussian blur in software on the
        // WebProcess main thread (CSSFilterRenderer -> Skia raster blur), and
        // gdb sampling during scroll showed it eating ~half the main thread:
        // device A/B scroll fps 2.9 -> 6.6 with the filters removed. Tailwind
        // blur/backdrop-blur utility classes cover all of Reddit's uses; a
        // constructable sheet adopted by the document AND every shadow root
        // (via the attachShadow hook) reaches the shadow-DOM components a
        // document-level user stylesheet cannot.
        try {
            var blurKill = new CSSStyleSheet();
            blurKill.replaceSync(
                '.post-background-image-filter,[class*="blur"],[style*="blur("]{' +
                'filter:none!important;' +
                '-webkit-backdrop-filter:none!important;' +
                'backdrop-filter:none!important}');
            function adoptBlurKill(root) {
                try { root.adoptedStyleSheets = root.adoptedStyleSheets.concat(blurKill); } catch (e) {}
            }
            adoptBlurKill(document);
            var origAttachShadow = Element.prototype.attachShadow;
            Element.prototype.attachShadow = function() {
                var root = origAttachShadow.apply(this, arguments);
                adoptBlurKill(root);
                return root;
            };
            // Shadow roots that already exist at install time (none at
            // DOCUMENT_START, but the script may be re-run on bfcache restore).
            (function adoptExisting(root) {
                if (!root || !root.querySelectorAll) return;
                var all = root.querySelectorAll('*');
                for (var i = 0; i < all.length; i++)
                    if (all[i].shadowRoot) {
                        adoptBlurKill(all[i].shadowRoot);
                        adoptExisting(all[i].shadowRoot);
                    }
            })(document);
        } catch (e) {}

        // Kill the "Get the app to keep using Reddit" xpromo bottom sheet.
        // uBO's rule for it is a :has-text() procedural selector adblock-rust
        // can't honor, and the cosmetic hide alone is not enough anyway: the
        // sheet also stamps body.rpl-scroll-lock (overflow:hidden), freezing
        // the page behind an invisible modal. Remove the sheet (light-DOM
        // child of shreddit-app, class .configured-xpromo, device-verified)
        // and lift the scroll lock whenever it (re)appears. The pre-paint
        // hide rule in atlantic-extra.txt prevents the first-paint flash.
        try {
            function killXpromo(root) {
                var nags = root.querySelectorAll('.configured-xpromo');
                for (var i = 0; i < nags.length; i++) {
                    try { nags[i].remove(); } catch (e) {}
                }
                if (nags.length && document.body)
                    document.body.classList.remove('rpl-scroll-lock');
            }
            new MutationObserver(function() { killXpromo(document); })
                .observe(document.documentElement, { childList: true, subtree: true });
            document.addEventListener('DOMContentLoaded',
                function() { killXpromo(document); }, { once: true });
        } catch (e) {}
    } catch (e) {}
})();
)JS";

// Cap MSE prebuffering. hls.js (the dominant MSE player on the web — Reddit,
// many embeds) keeps filling its SourceBuffer up to maxBufferLength /
// maxMaxBufferLength / maxBufferSize *regardless of whether the <video> is
// paused*, so a gated/paused feed video buffers the whole clip (tens of MB of
// resident media that kRedditPerf's pause() does NOT stop — hls.js buffers
// independently of play state). These are SUPPORTED hls.js config knobs, so
// clamping them just makes the player buffer less ahead; it does not break
// playback (12 s of read-ahead is ample). Site-agnostic — bounded prebuffer is
// broadly good on a 3.5 GB device. Limitation: only reaches builds that expose
// the global `window.Hls` (UMD/CDN); ES-module imports are not interceptable
// from a user script. Native/dash.js/Shaka MSE is out of scope for now.
// hls.js defaults: maxBufferLength=30, maxMaxBufferLength=600, maxBufferSize=60M,
// backBufferLength=Infinity. Set ATLANTIC_DISABLE_MEDIA_BUFCAP=1 to disable.
static const char* const kMediaBufferCap = R"JS(
(function() {
    try {
        if (window.__wpeMediaBufCapInstalled) return;
        window.__wpeMediaBufCapInstalled = true;

        var CAP = { maxBufferLength: 12, maxMaxBufferLength: 30,
                    maxBufferSize: 20 * 1000 * 1000, backBufferLength: 10 };
        function clampConfig(cfg) {
            cfg = cfg || {};
            try {
                for (var k in CAP) {
                    if (typeof cfg[k] !== 'number' || cfg[k] > CAP[k]) cfg[k] = CAP[k];
                }
            } catch (e) {}
            return cfg;
        }
        function patchHls(H) {
            if (!H || H.__wpeCapped) return H;
            try {
                if (H.DefaultConfig) clampConfig(H.DefaultConfig);
                // Wrap the constructor so an explicit per-instance config is
                // clamped too; inherit hls.js statics (Events, isSupported,
                // DefaultConfig, ...) via the prototype chain.
                var Wrapped = function(userCfg) { return new H(clampConfig(userCfg)); };
                Wrapped.prototype = H.prototype;
                Object.setPrototypeOf(Wrapped, H);
                Wrapped.__wpeCapped = true;
                return Wrapped;
            } catch (e) { return H; }
        }

        var _Hls = window.Hls ? patchHls(window.Hls) : undefined;
        try {
            Object.defineProperty(window, 'Hls', {
                configurable: true,
                get: function() { return _Hls; },
                set: function(v) { _Hls = patchHls(v); }
            });
        } catch (e) { if (window.Hls) window.Hls = _Hls; }
    } catch (e) {}
})();
)JS";

// Force HW-decodable video on YouTube (h264ify technique). The Xperia 10 II HW
// decoder (droidvdec) accelerates H.264/H.265; VP8/VP9/AV1 fall back to SOFTWARE
// (vp9dec/vpxdec), which cannot keep up with YouTube's 1080p VP9 (device-measured
// ~0 fps decode, the picture barely advances). YouTube's HTML5 player chooses its
// codec from MediaSource.isTypeSupported / HTMLMediaElement.canPlayType /
// navigator.mediaCapabilities.decodingInfo — NOT from the User-Agent (verified on
// device: an iPhone UA still got VP9) — and it always offers an avc1 (H.264)
// adaptive format alongside vp9/av01. So we make those three probes report
// vp9/vp8/av01 as unsupported; the player then picks avc1, which droidvdec
// hardware-decodes (device-verified: decode path flips vpx → droidvdec0:src).
// Trade-off: H.264 on YouTube caps at 1080p (no VP9/AV1 1440p+), which matches
// this device's panel and the droidvdec 1080p ceiling. DOCUMENT_START so the
// overrides are in place before the player's player.js probes codecs; all frames
// since the player can be iframed (youtube-nocookie embeds). Self-scoped to
// YouTube hosts. Set ATLANTIC_DISABLE_YT_H264=1 to disable.
static const char* const kYouTubeH264 = R"JS(
(function() {
    try {
        if (!/(^|\.)youtube\.com$|(^|\.)youtube-nocookie\.com$|(^|\.)youtu\.be$/i.test(location.hostname))
            return;
        if (window.__wpeYtH264Installed) return;
        window.__wpeYtH264Installed = true;

        var BLOCKED = /vp0?9|vp0?8|av01/i;

        function wrapIsTypeSupported(ctor) {
            try {
                if (!ctor || !ctor.isTypeSupported || ctor.__wpeH264) return;
                var orig = ctor.isTypeSupported;
                ctor.isTypeSupported = function(type) {
                    return BLOCKED.test(String(type)) ? false : orig.call(ctor, type);
                };
                ctor.__wpeH264 = true;
            } catch (e) {}
        }
        wrapIsTypeSupported(window.MediaSource);
        wrapIsTypeSupported(window.WebKitMediaSource);
        wrapIsTypeSupported(window.ManagedMediaSource);

        try {
            var cpt = HTMLMediaElement.prototype.canPlayType;
            HTMLMediaElement.prototype.canPlayType = function(type) {
                return BLOCKED.test(String(type)) ? "" : cpt.call(this, type);
            };
        } catch (e) {}

        try {
            var mc = navigator.mediaCapabilities;
            if (mc && mc.decodingInfo) {
                var di = mc.decodingInfo.bind(mc);
                mc.decodingInfo = function(cfg) {
                    try {
                        var ct = cfg && cfg.video && cfg.video.contentType;
                        if (ct && BLOCKED.test(ct))
                            return Promise.resolve({ supported: false, smooth: false, powerEfficient: false });
                    } catch (e) {}
                    return di(cfg);
                };
            }
        } catch (e) {}
    } catch (e) {}
})();
)JS";

// Twitch playback fix. Paired with the Android-Chrome UA quirk in WPEWebPage
// (which gets Twitch onto its MSE player so WPE can decode the stream — see
// atlanticUserAgentForUrl), this clears the two remaining gates that stop a
// stream from actually starting on this device:
//
//  1. Autoplay. Twitch calls video.play() on load with sound, but the engine's
//     media_playback_requires_user_gesture policy rejects AUDIBLE autoplay
//     (UserGestureRequired), and Twitch's player tears the MediaSource down on
//     that rejection — so the picture never appears. MUTED autoplay IS permitted
//     (same policy that lets Reddit's muted feed videos play). So force the
//     player <video> muted whenever play() is called WITHOUT a transient user
//     activation: autoplay then starts muted (picture shows immediately) and the
//     user unmutes with a tap. A real tap keeps its activation, so intentional
//     audible play (and unmute) is untouched — this only catches the automatic
//     load-time autoplay, exactly like a desktop browser's autoplay-muted.
//
//  2. The "Open in App / Keep using web" interstitial. Twitch's mobile web shows
//     an app-install gate to mobile-Chrome UAs that pauses the page until it is
//     dismissed (device-observed: the stream uptime freezes behind it). Click
//     its "Keep using web" affordance so playback proceeds. Controls render late
//     and the gate can reappear, so a MutationObserver + short interval keep
//     watching. Self-scoped to Twitch hosts. Set ATLANTIC_DISABLE_TWITCH=1 to
//     disable the whole script.
static const char* const kTwitch = R"JS(
(function() {
    try {
        if (!/(^|\.)twitch\.tv$/i.test(location.hostname)) return;
        if (window.__wpeTwitchInstalled) return;
        window.__wpeTwitchInstalled = true;

        function userInitiated() {
            return !!(navigator.userActivation && navigator.userActivation.isActive);
        }

        // Force muted autoplay so the engine's gesture policy permits it; a real
        // user tap (which carries an activation) still plays/unmutes with sound.
        var origPlay = HTMLMediaElement.prototype.play;
        HTMLMediaElement.prototype.play = function() {
            try {
                if (this.tagName === 'VIDEO' && !userInitiated() && !this.muted)
                    this.muted = true;
            } catch (e) {}
            return origPlay.apply(this, arguments);
        };

        // Dismiss Twitch's "Open in App / Keep using web" gate so it stops
        // pausing the page. Find the smallest clickable element whose text is the
        // "keep using web" affordance and click it.
        function dismissAppGate() {
            var nodes = document.querySelectorAll(
                'a, button, [role="button"], [tabindex], div, span');
            for (var i = 0; i < nodes.length; i++) {
                var el = nodes[i];
                if (el.__wpeTwitchGateTried) continue;
                var txt = (el.textContent || '').trim();
                if (txt.length > 40) continue;
                if (/^(keep using web|continue (on|in) (web|browser)|not now)$/i.test(txt)) {
                    el.__wpeTwitchGateTried = true;
                    try { el.click(); } catch (e) {}
                    return true;
                }
            }
            return false;
        }

        var pending = false;
        function schedule() {
            if (pending) return;
            pending = true;
            var run = function() { pending = false; dismissAppGate(); };
            if (window.requestAnimationFrame) requestAnimationFrame(run);
            else setTimeout(run, 16);
        }

        var mo = new MutationObserver(schedule);
        function start() {
            mo.observe(document.documentElement, { childList: true, subtree: true });
            dismissAppGate();
        }
        if (document.documentElement) start();
        else document.addEventListener('DOMContentLoaded', start, { once: true });
        var ticks = 0;
        var timer = setInterval(function() {
            dismissAppGate();
            if (++ticks > 40) clearInterval(timer);   // ~20s safety net, then stop
        }, 500);
    } catch (e) {}
})();
)JS";

static const char* const kScrollBridge = R"JS(
(function() {
    if (window.__wpeScrollBridgeInstalled) return;
    window.__wpeScrollBridgeInstalled = true;

    var pending = false;
    function report() {
        pending = false;
        try {
            window.webkit.messageHandlers.scrollBridge.postMessage({
                scrollY:      window.scrollY,
                scrollHeight: document.documentElement.scrollHeight,
                innerHeight:  window.innerHeight
            });
        } catch(e) {}
    }
    window.addEventListener('scroll', function() {
        if (!pending) {
            pending = true;
            setTimeout(report, 100);
        }
    }, {passive: true, capture: true});
})();
)JS";

static const char* const kMediaBridge = R"JS(
(function() {
    if (window.__wpeMediaBridgeInstalled) return;
    window.__wpeMediaBridgeInstalled = true;
    var explicitFullscreenActive = false;
    var lastActiveTimestamp = 0;
    var lastTriggerEvent = null;

    function isMediaElement(el) {
        if (!el || !el.tagName)
            return false;
        var tag = el.tagName.toLowerCase();
        return tag === 'audio' || tag === 'video';
    }

    function detectFullscreenState() {
        if (document.fullscreenElement || document.webkitFullscreenElement)
            return true;

        var videos = document.querySelectorAll ? document.querySelectorAll('video') : [];
        for (var i = 0; i < videos.length; i++) {
            if (videos[i] && videos[i].webkitDisplayingFullscreen)
                return true;
        }
        return false;
    }

    function updateExplicitFullscreenState(event) {
        if (event) {
            if (event.type === 'webkitbeginfullscreen') {
                // Cancel any pending clear and mark fullscreen active.
                if (window.__wpeEndFullscreenTimer) {
                    clearTimeout(window.__wpeEndFullscreenTimer);
                    window.__wpeEndFullscreenTimer = null;
                }
                explicitFullscreenActive = true;
                return;
            }
            if (event.type === 'webkitendfullscreen') {
                // Intentionally ignore: webkitendfullscreen fires spuriously on WPE
                // during the viewport resize that accompanies fullscreen entry.
                // Real exits are signaled by C++ (leaveFullscreenRequested) injecting
                // window.__wpeClearExplicitFullscreen(), or by fullscreenchange.
                return;
            }
        }
        explicitFullscreenActive = detectFullscreenState();
    }

    // C++ (leaveFullscreenRequested) calls this to signal a real fullscreen exit.
    window.__wpeClearExplicitFullscreen = function() {
        if (window.__wpeEndFullscreenTimer) {
            clearTimeout(window.__wpeEndFullscreenTimer);
            window.__wpeEndFullscreenTimer = null;
        }
        explicitFullscreenActive = false;
        scheduleMediaState(null);
    };

    function normalizedDesiredVolume() {
        var volume = window.__wpeDesiredMediaVolume;
        if (typeof volume !== 'number' || !isFinite(volume))
            return null;
        return Math.max(0.0, Math.min(1.0, volume));
    }

    function applyDesiredMediaState(el) {
        if (!isMediaElement(el))
            return;

        var desiredVolume = normalizedDesiredVolume();
        if (desiredVolume !== null && el.volume !== desiredVolume)
            el.volume = desiredVolume;

        if (typeof window.__wpeDesiredMediaMuted === 'boolean' && el.muted !== window.__wpeDesiredMediaMuted)
            el.muted = window.__wpeDesiredMediaMuted;
    }

    function applyDesiredMediaStateToNode(node) {
        if (!node)
            return;

        if (isMediaElement(node))
            applyDesiredMediaState(node);

        if (!node.querySelectorAll)
            return;

        var media = node.querySelectorAll('audio,video');
        for (var i = 0; i < media.length; i++)
            applyDesiredMediaState(media[i]);
    }

    window.__wpeApplyDesiredMediaStateToMedia = applyDesiredMediaStateToNode;

    function mediaStatePayload() {
        var media = document.querySelectorAll('audio,video');
        var audioActive = false;
        var videoActive = false;
        var fullscreenActive = explicitFullscreenActive || detectFullscreenState();
        var volume = 1.0;
        var muted = false;
        var preferredMedia = null;

        for (var i = 0; i < media.length; i++) {
            var el = media[i];
            if (!isMediaElement(el))
                continue;

            applyDesiredMediaState(el);
            if (!preferredMedia || (!el.paused && !el.ended))
                preferredMedia = el;
            if (!fullscreenActive && el.tagName.toLowerCase() === 'video') {
                fullscreenActive = !!el.webkitDisplayingFullscreen;
            }
            if (!el.paused && !el.ended) {
                if (el.tagName.toLowerCase() === 'video') {
                    videoActive = true;
                    audioActive = true;
                } else if (!el.muted) {
                    // A playing, non-muted <audio> counts as audio-active even at
                    // volume 0, so the native-volume poll keeps running and the
                    // slider can bring it back up. Gating this on volume > 0 let a
                    // slide-to-zero stop the poll and freeze the element silent
                    // (volume-up could never recover it). Video is already handled
                    // above regardless of volume.
                    audioActive = true;
                }
            }
        }

        if (preferredMedia && preferredMedia.volume !== undefined) {
            volume = preferredMedia.volume;
            muted = preferredMedia.muted;
        }

        var definitiveInactive = lastTriggerEvent === 'pause'
            || lastTriggerEvent === 'ended'
            || lastTriggerEvent === 'emptied';
        if (audioActive) {
            lastActiveTimestamp = Date.now();
        } else if (lastTriggerEvent === 'waiting'
                   || lastTriggerEvent === 'stall'
                   || lastTriggerEvent === 'stalled') {
            audioActive = true;
        } else if (!definitiveInactive && (Date.now() - lastActiveTimestamp) < 2000) {
            audioActive = true;
        }

        return {
            type: 'state',
            audioActive: audioActive,
            videoActive: videoActive,
            fullscreenActive: fullscreenActive,
            volume: volume,
            muted: muted,
            volumeChangedByPage: lastTriggerEvent === 'volumechange'
        };
    }

    // While media is active, re-post the state every 2 s so the native side
    // notices silent stops (a playing element removed from the DOM pauses
    // without firing any media event). Replaces the old document-wide
    // MutationObserver, which re-scanned the DOM and posted an IPC message on
    // every mutation batch — a constant tax on mutation-heavy SPAs.
    var activeKeepalive = null;
    function postMediaState() {
        var payload = null;
        try {
            payload = mediaStatePayload();
            window.webkit.messageHandlers.mediaBridge.postMessage(payload);
        } catch (ex) {
            console.error('[WPE-MEDIA-JS] postMessage error: ' + ex);
        }
        lastTriggerEvent = null;
        var active = !!(payload && (payload.audioActive || payload.videoActive));
        if (active && !activeKeepalive) {
            activeKeepalive = setInterval(postMediaState, 2000);
        } else if (!active && activeKeepalive) {
            clearInterval(activeKeepalive);
            activeKeepalive = null;
        }
    }

    window.__wpePostMediaState = postMediaState;

    var events = [
        'play',
        'playing',
        'pause',
        'ended',
        'emptied',
        'volumechange',
        'loadeddata',
        'loadedmetadata',
        'seeking',
        'seeked'
    ];

    function onMediaEvent(event) {
        // Media events don't bubble but ARE seen here in the capture phase,
        // for any media element — including ones added to the DOM after load.
        // Applying the desired volume/mute at the moment an element does
        // something audible replaces the old MutationObserver scan.
        if (event && isMediaElement(event.target))
            applyDesiredMediaState(event.target);
        scheduleMediaState(event);
    }

    for (var i = 0; i < events.length; i++) {
        document.addEventListener(events[i], onMediaEvent, true);
    }

    function onFullscreenEvent(event) {
        updateExplicitFullscreenState(event);
        scheduleMediaState(event);
    }

    document.addEventListener('fullscreenchange', onFullscreenEvent, true);
    document.addEventListener('webkitfullscreenchange', onFullscreenEvent, true);
    document.addEventListener('webkitbeginfullscreen', onFullscreenEvent, true);
    document.addEventListener('webkitendfullscreen', onFullscreenEvent, true);

    // Debounced media state posting: collapse rapid-fire DOM and media events into
    // a single async querySelectorAll + IPC round-trip per turn.
    var mediaBridgePending = false;
    function scheduleMediaState(event) {
        if (event && event.type)
            lastTriggerEvent = event.type;
        else if (!mediaBridgePending)
            lastTriggerEvent = null;
        if (!mediaBridgePending) {
            mediaBridgePending = true;
            setTimeout(function() {
                mediaBridgePending = false;
                postMediaState();
            }, 0);
        }
    }

    // NOTE: no MutationObserver here on purpose. The previous implementation
    // observed {childList, subtree} on the whole document in every frame and,
    // per mutation batch, queued one setTimeout per added node (each running
    // querySelectorAll) plus a full document scan + mediaBridge IPC post. On
    // mutation-heavy pages (React/Vue SPAs, infinite feeds) that ran thousands
    // of times during load and continuously afterwards. New media elements are
    // instead caught by the capture-phase media event listeners above the
    // moment they load/play, and silent removals by the active keepalive.
    updateExplicitFullscreenState();
    document.addEventListener('DOMContentLoaded', postMediaState, true);
    applyDesiredMediaStateToNode(document);
    postMediaState();
})();
)JS";

static const char* const kSelectBridge = R"JS(
(function() {
    if (window.__wpeSelectBridgeInstalled) return;
    window.__wpeSelectBridgeInstalled = true;
    console.error('[WPE-SELECT-JS] selectBridge JS installed, handlers=' +
        (window.webkit && window.webkit.messageHandlers ? 'YES' : 'NO'));

    function handleSelectActivation(e) {
        var el = e.target;
        while (el && el.tagName && el.tagName.toLowerCase() !== 'select') {
            el = el.parentElement;
        }
        if (!el || !el.tagName || el.tagName.toLowerCase() !== 'select') return;
        console.error('[WPE-SELECT-JS] intercepted ' + e.type + ' on <select>, options=' + el.options.length);
        e.preventDefault();
        e.stopImmediatePropagation();
        var opts = [];
        for (var i = 0; i < el.options.length; i++) {
            opts.push(el.options[i].text);
        }
        window.__wpePendingSelect = el;
        try {
            window.webkit.messageHandlers.selectBridge.postMessage({
                options: opts,
                selectedIndex: el.selectedIndex
            });
            console.error('[WPE-SELECT-JS] postMessage sent');
        } catch(ex) {
            console.error('[WPE-SELECT-JS] postMessage error: ' + ex);
        }
    }
    document.addEventListener('mousedown',   handleSelectActivation, true);
    document.addEventListener('touchstart',  handleSelectActivation, {capture: true, passive: true});
    document.addEventListener('pointerdown', handleSelectActivation, true);
    document.addEventListener('click',       handleSelectActivation, true);
    console.error('[WPE-SELECT-JS] event listeners registered');
})();
)JS";

// Generic cosmetic ad blocking: the adblock engine's generic rules
// (##.ad-banner and friends) are keyed by the class/id names actually present
// in the DOM (Engine::hidden_class_id_selectors). This collector reports each
// class/id name once to the UI process ("adblockClassId" handler), batched via
// MutationObserver so infinite-scroll / SPA-inserted ads are covered too. The
// UI process answers by injecting the matching hide rules (see
// onAdblockClassIdMessage). Batching (150ms) + send-once dedup + a hard cap
// keep the observer cheap on mutation-heavy pages.
static const char* const kAdblockClassIdCollector = R"JS(
(function() {
    if (window.__atlAdblockClassId) return;
    window.__atlAdblockClassId = true;
    var sent = Object.create(null), qc = [], qi = [], timer = null;
    var total = 0, MAX = 50000;
    function one(e) {
        if (total > MAX || !e || !e.getAttribute) return;
        var id = e.id;
        if (id && typeof id === 'string' && !sent['#' + id]) {
            sent['#' + id] = 1; qi.push(id); total++;
        }
        var cl = e.classList;
        if (cl) for (var j = 0; j < cl.length; j++) {
            var c = cl[j];
            if (!sent['.' + c]) { sent['.' + c] = 1; qc.push(c); total++; }
        }
    }
    function walk(root) {
        if (!root || root.nodeType !== 1) return;
        one(root);
        if (!root.querySelectorAll) return;
        var els = root.querySelectorAll('[class],[id]');
        for (var i = 0; i < els.length; i++) one(els[i]);
    }
    function flush() {
        timer = null;
        if (!qc.length && !qi.length) return;
        var msg = { c: qc, i: qi };
        qc = []; qi = [];
        try { window.webkit.messageHandlers.adblockClassId.postMessage(msg); } catch (e) {}
    }
    function sched() { if (!timer) timer = setTimeout(flush, 150); }
    var mo = new MutationObserver(function(muts) {
        for (var i = 0; i < muts.length; i++) {
            var m = muts[i];
            if (m.type === 'attributes') one(m.target);
            else for (var j = 0; j < m.addedNodes.length; j++) walk(m.addedNodes[j]);
        }
        if (qc.length || qi.length) sched();
    });
    function start() {
        walk(document.documentElement);
        mo.observe(document.documentElement, {
            childList: true, subtree: true,
            attributes: true, attributeFilter: ['class', 'id']
        });
        sched();
    }
    if (document.readyState === 'loading')
        document.addEventListener('DOMContentLoaded', start);
    else
        start();
})();
)JS";

} // namespace WPEUserScripts
