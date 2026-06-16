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

    function beginLongPress(x, y) {
        cancelLongPress();
        longPressPoint = { x: x, y: y };
        longPressStartPoint = { x: x, y: y };
        longPressTimer = setTimeout(function() {
            longPressTimer = null;
            if (!longPressPoint) return;
            var lx = longPressPoint.x, ly = longPressPoint.y;
            longPressPoint = null;
            var imgUrl = imageUrlAtPoint(lx, ly);
            if (imgUrl) {
                try {
                    window.webkit.messageHandlers.imageLongPressBridge.postMessage(
                        { imageUrl: imgUrl, x: lx, y: ly });
                } catch(e) {}
                return;
            }
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

// YouTube player control icons are inline <svg><path class="ytp-svg-fill">, not
// CSS masks. On WPE 2.52 their computed fill is the SVG default black (YouTube's
// intended white never applies — its fill rule resolves to an unset custom
// property), so the controls are invisible on the dark player. Force the fill
// white. Diagnosed on-device via the remote inspector: computed fill goes
// rgb(0,0,0) -> rgb(255,255,255) the moment this rule is present. One stable
// class, language-agnostic, robust across redesigns — unlike the old per-icon
// aria-label/hand-drawn-SVG override. (The generic mask healer below does not
// help here: YouTube uses inline SVG, not url() masks.)
static const char* const kYouTubeIconFix = R"JS(
(function() {
    if (document.getElementById('__wpe_yt_icon_fix')) return;
    var s = document.createElement('style');
    s.id = '__wpe_yt_icon_fix';
    s.textContent = '.ytp-svg-fill{fill:#fff !important;}';
    (document.head || document.documentElement).appendChild(s);
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
    document.addEventListener('pointerup', function() {
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
        // Capture phase sees play events for every <video>, including those in
        // shadow DOM (shreddit-player) and ones added after load — no observer.
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

} // namespace WPEUserScripts
