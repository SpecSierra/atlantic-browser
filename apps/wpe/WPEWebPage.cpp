/*
 * Copyright (c) 2024 Jolla Ltd.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "WPEWebPage.h"

#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineF>
#include <QPointer>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <array>

#include "WPEQtViewLoadRequest.h"

#include <wpe/webkit.h>
#include <gio/gio.h>

namespace {

constexpr double kMinimumPinchZoomFactor = 0.5;
constexpr double kMaximumPinchZoomFactor = 3.0;

QStringList wildcardFiltersForFamily(const QString &family)
{
    if (family == QStringLiteral("image")) {
        return {
            QStringLiteral("*.png"),
            QStringLiteral("*.jpg"),
            QStringLiteral("*.jpeg"),
            QStringLiteral("*.gif"),
            QStringLiteral("*.bmp"),
            QStringLiteral("*.webp"),
            QStringLiteral("*.svg"),
            QStringLiteral("*.heif"),
            QStringLiteral("*.heic")
        };
    }
    if (family == QStringLiteral("audio")) {
        return {
            QStringLiteral("*.mp3"),
            QStringLiteral("*.aac"),
            QStringLiteral("*.m4a"),
            QStringLiteral("*.ogg"),
            QStringLiteral("*.wav"),
            QStringLiteral("*.flac")
        };
    }
    if (family == QStringLiteral("video")) {
        return {
            QStringLiteral("*.mp4"),
            QStringLiteral("*.m4v"),
            QStringLiteral("*.webm"),
            QStringLiteral("*.mkv"),
            QStringLiteral("*.mov"),
            QStringLiteral("*.avi"),
            QStringLiteral("*.3gp")
        };
    }
    if (family == QStringLiteral("text")) {
        return {
            QStringLiteral("*.txt"),
            QStringLiteral("*.md"),
            QStringLiteral("*.csv"),
            QStringLiteral("*.json"),
            QStringLiteral("*.xml"),
            QStringLiteral("*.html"),
            QStringLiteral("*.htm")
        };
    }
    return {};
}

QStringList mimeToFilters(const QString &mimeType)
{
    const QString mime = mimeType.trimmed().toLower();
    if (mime.isEmpty() || mime == QStringLiteral("*") || mime == QStringLiteral("*/*")) {
        return {};
    }

    const int slash = mime.indexOf(QLatin1Char('/'));
    if (slash <= 0 || slash >= mime.size() - 1) {
        return {};
    }

    const QString family = mime.left(slash);
    const QString subtype = mime.mid(slash + 1);

    if (subtype == QStringLiteral("*")) {
        return wildcardFiltersForFamily(family);
    }

    if (subtype == QStringLiteral("jpeg")) {
        return { QStringLiteral("*.jpg"), QStringLiteral("*.jpeg") };
    }
    if (subtype == QStringLiteral("svg+xml")) {
        return { QStringLiteral("*.svg") };
    }
    if (subtype == QStringLiteral("x-m4a")) {
        return { QStringLiteral("*.m4a") };
    }
    if (subtype == QStringLiteral("quicktime")) {
        return { QStringLiteral("*.mov") };
    }

    return { QStringLiteral("*.%1").arg(subtype) };
}

QStringList requestNameFilters(WebKitFileChooserRequest *request)
{
    if (!request) {
        return {};
    }

    const gchar* const* mimeTypes = webkit_file_chooser_request_get_mime_types(request);
    if (!mimeTypes || !mimeTypes[0]) {
        return {};
    }

    QSet<QString> filters;
    for (guint i = 0; mimeTypes[i]; ++i) {
        const QString mime = QString::fromUtf8(mimeTypes[i]).trimmed();
        if (mime.isEmpty() || mime == QStringLiteral("*") || mime == QStringLiteral("*/*")) {
            return {};
        }
        const QStringList mapped = mimeToFilters(mime);
        if (mapped.isEmpty()) {
            return {};
        }
        for (const QString &entry : mapped) {
            filters.insert(entry);
        }
    }

    QStringList ordered = filters.values();
    std::sort(ordered.begin(), ordered.end());
    return ordered;
}

QList<QTouchEvent::TouchPoint> mergeTrackedTouchPoints(
        QHash<int, QTouchEvent::TouchPoint> &trackedTouchPoints,
        const QList<QTouchEvent::TouchPoint> &touchPoints,
        QEvent::Type eventType)
{
    if (eventType == QEvent::TouchBegin && touchPoints.size() == 1) {
        trackedTouchPoints.clear();
    }

    for (const QTouchEvent::TouchPoint &touchPoint : touchPoints) {
        if (touchPoint.state() == Qt::TouchPointReleased) {
            trackedTouchPoints.remove(touchPoint.id());
        } else {
            trackedTouchPoints.insert(touchPoint.id(), touchPoint);
        }
    }

    if (eventType == QEvent::TouchCancel) {
        trackedTouchPoints.clear();
        return {};
    }

    QList<QTouchEvent::TouchPoint> activePoints = trackedTouchPoints.values();
    std::sort(activePoints.begin(), activePoints.end(), [](const QTouchEvent::TouchPoint &lhs, const QTouchEvent::TouchPoint &rhs) {
        return lhs.id() < rhs.id();
    });
    return activePoints;
}

void resetPinchZoomState(bool &pinchZoomActive, qreal &pinchStartDistance, double &pinchStartZoomLevel)
{
    pinchZoomActive = false;
    pinchStartDistance = 0.0;
    pinchStartZoomLevel = 1.0;
}

struct KeyboardProbeData {
    QPointer<WPEWebPage> page;
};

void onKeyboardProbeEvaluated(GObject* object, GAsyncResult* result, gpointer userData)
{
    std::unique_ptr<KeyboardProbeData> data(static_cast<KeyboardProbeData*>(userData));
    if (!data || !data->page)
        return;

    GError* error = nullptr;
    JSCValue* value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(object), result, &error);
    if (!value) {
        if (error) {
            g_error_free(error);
        }
        return;
    }

    bool editable = false;
    if (jsc_value_is_boolean(value)) {
        editable = jsc_value_to_boolean(value);
    }
    g_object_unref(value);

    QInputMethod* inputMethod = QGuiApplication::inputMethod();
    if (!inputMethod)
        return;

    if (editable) {
        inputMethod->show();
    } else if (inputMethod->isVisible()) {
        inputMethod->hide();
    }
}

gboolean onDecidePolicy(WebKitWebView* webView, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer)
{
    static const std::array<const char*, 20> blockedHostSuffixes = {{
        "doubleclick.net",
        "googlesyndication.com",
        "googleadservices.com",
        "googletagservices.com",
        "googletagmanager.com",
        "google-analytics.com",
        "adservice.google.com",
        "adnxs.com",
        "taboola.com",
        "outbrain.com",
        "criteo.com",
        "rubiconproject.com",
        "openx.net",
        "pubmatic.com",
        "advertising.com",
        "amazon-adsystem.com",
        "scorecardresearch.com",
        "quantserve.com",
        "moatads.com",
        "connect.facebook.net"
    }};

    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        WebKitResponsePolicyDecision* responseDecision = WEBKIT_RESPONSE_POLICY_DECISION(decision);
        if (!responseDecision || webkit_response_policy_decision_is_main_frame_main_resource(responseDecision))
            return FALSE;

        WebKitURIRequest* request = webkit_response_policy_decision_get_request(responseDecision);
        const gchar* uri = request ? webkit_uri_request_get_uri(request) : nullptr;
        if (!uri || !*uri)
            return FALSE;

        const QUrl parsedUrl(QString::fromUtf8(uri));
        const QString host = parsedUrl.host().toLower();
        if (host.isEmpty())
            return FALSE;

        for (const char* suffixRaw : blockedHostSuffixes) {
            const QString suffix = QString::fromLatin1(suffixRaw);
            if (host == suffix || host.endsWith(QStringLiteral(".") + suffix)) {
                fprintf(stderr, "[WPE-ADBLOCK] blocked %s\n", uri);
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }
        }

        return FALSE;
    }

    if (type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        return FALSE;
    }

    WebKitNavigationPolicyDecision* navigationDecision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
    WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(navigationDecision);
    WebKitURIRequest* request = action ? webkit_navigation_action_get_request(action) : nullptr;
    const gchar* uri = request ? webkit_uri_request_get_uri(request) : nullptr;
    if (uri && *uri) {
        webkit_web_view_load_uri(webView, uri);
    }
    webkit_policy_decision_ignore(decision);
    return TRUE;
}

static void onSelectBridgeMessage(WebKitUserContentManager*, JSCValue* value, gpointer userData)
{
    WPEWebPage* page = static_cast<WPEWebPage*>(userData);
    if (!page || !value)
        return;

    gchar* json = jsc_value_to_json(value, 0);
    if (!json)
        return;

    QByteArray jsonBytes(json);
    g_free(json);

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return;

    QJsonObject obj = doc.object();
    QJsonArray optArray = obj.value(QStringLiteral("options")).toArray();
    int selectedIndex = obj.value(QStringLiteral("selectedIndex")).toInt(0);

    QStringList items;
    items.reserve(optArray.size());
    for (const QJsonValue &v : optArray)
        items.append(v.toString());

    page->openSelectMenu(items, selectedIndex);
}

static void onSelectionBridgeMessage(WebKitUserContentManager*, JSCValue* value, gpointer userData)
{
    WPEWebPage* page = static_cast<WPEWebPage*>(userData);
    if (!page || !value)
        return;

    gchar* json = jsc_value_to_json(value, 0);
    if (!json)
        return;

    QByteArray jsonBytes(json);
    g_free(json);

    QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (!doc.isObject())
        return;

    const QJsonObject obj = doc.object();
    const QString type = obj.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("clear")) {
        page->handleJsSelectionClear();
    } else if (type == QLatin1String("select")) {
        page->handleJsSelectionUpdate(
            obj.value(QStringLiteral("text")).toString(),
            obj.value(QStringLiteral("sx")).toDouble(),
            obj.value(QStringLiteral("sy")).toDouble(),
            obj.value(QStringLiteral("ex")).toDouble(),
            obj.value(QStringLiteral("ey")).toDouble());
    }
}

static void onSelectionBridgeInstall(WebKitUserContentManager* ucm, WPEWebPage* page)
{
    g_signal_connect(ucm, "script-message-received::selectionBridge",
                     G_CALLBACK(onSelectionBridgeMessage), page);
    webkit_user_content_manager_register_script_message_handler(ucm, "selectionBridge", nullptr);

    static const gchar* selectionBridgeJs = R"JS(
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

        var anchor = caretRect(sel.anchorNode, sel.anchorOffset);
        var focus = caretRect(sel.focusNode, sel.focusOffset);
        var range = sel.getRangeAt(0);
        var bounds = range.getBoundingClientRect();
        var rects = range.getClientRects();
        var firstRect = rects && rects.length ? rects[0] : null;
        var lastRect = rects && rects.length ? rects[rects.length - 1] : null;

        var sx = firstRect ? firstRect.left : (anchor ? anchor.left : (bounds ? bounds.left : 0));
        var sy = firstRect ? firstRect.bottom : (anchor ? anchor.bottom : (bounds ? bounds.bottom : 0));
        var ex = lastRect ? lastRect.right : (focus ? focus.right : (bounds ? bounds.right : 0));
        var ey = lastRect ? lastRect.bottom : (focus ? focus.bottom : (bounds ? bounds.bottom : 0));

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

    function postSelection() {
        try {
            window.webkit.messageHandlers.selectionBridge.postMessage(selectionPayload());
        } catch (ex) {
            console.error('[WPE-SEL-JS] postMessage error: ' + ex);
        }
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

    function beginLongPress(x, y) {
        cancelLongPress();
        longPressPoint = { x: x, y: y };
        longPressStartPoint = { x: x, y: y };
        longPressTimer = setTimeout(function() {
            longPressTimer = null;
            if (longPressPoint && selectWordAtPoint(longPressPoint.x, longPressPoint.y))
                postSelection();
            longPressPoint = null;
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
    document.addEventListener('mouseup', postSelection, true);
    document.addEventListener('touchend', postSelection, true);
    document.addEventListener('keyup', postSelection, true);
    document.addEventListener('contextmenu', function(e) {
        if (selectWordAtPoint(e.clientX, e.clientY)) {
            e.preventDefault();
            postSelection();
        }
    }, true);
    document.addEventListener('touchstart', function(e) {
        var p = touchPointFromEvent(e);
        if (p)
            beginLongPress(p.clientX, p.clientY);
    }, true);
    document.addEventListener('touchmove', function(e) {
        var p = touchPointFromEvent(e);
        if (!p || !longPressStartPoint)
            return;
        var dx = p.clientX - longPressStartPoint.x;
        var dy = p.clientY - longPressStartPoint.y;
        if ((dx * dx) + (dy * dy) > longPressMoveThreshold * longPressMoveThreshold)
            cancelLongPress();
    }, true);
    document.addEventListener('touchcancel', cancelLongPress, true);
    document.addEventListener('touchend', cancelLongPress, true);
})();
)JS";

    WebKitUserScript* script = webkit_user_script_new(
        selectionBridgeJs,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);
}

bool dispatchTextToFocusedElement(WPEWebPage* page, const QString& text, int replaceBeforeCaret)
{
    if (!page)
        return false;

    if (replaceBeforeCaret < 0)
        replaceBeforeCaret = 0;

    const QString args = QString::fromUtf8(QJsonDocument(QJsonArray{ text, replaceBeforeCaret }).toJson(QJsonDocument::Compact));
    const QString script = QStringLiteral(
        "(function(args){"
        "  var t=args[0];"
        "  var replaceBefore=(args[1]||0)|0;"
        "  function isEditable(e){"
        "    if(!e) return false;"
        "    if(e.isContentEditable) return true;"
        "    var tag=(e.tagName||'').toLowerCase();"
        "    if(tag==='textarea') return !e.readOnly && !e.disabled;"
        "    if(tag==='input'){"
        "      var tp=(e.type||'text').toLowerCase();"
        "      var blocked={button:1,submit:1,reset:1,checkbox:1,radio:1,file:1,image:1,range:1,color:1,hidden:1};"
        "      return !blocked[tp] && !e.readOnly && !e.disabled;"
        "    }"
        "    return false;"
        "  }"
        "  function fireInput(el,data,inputType){"
        "    try {"
        "      if (typeof InputEvent === 'function')"
        "        el.dispatchEvent(new InputEvent('input',{bubbles:true,data:data,inputType:inputType}));"
        "      else"
        "        el.dispatchEvent(new Event('input',{bubbles:true}));"
        "    } catch(_e) {"
        "      el.dispatchEvent(new Event('input',{bubbles:true}));"
        "    }"
        "  }"
        "  var el=document.activeElement;"
        "  if(!isEditable(el)) return false;"
        "  if(el.isContentEditable){"
        "    var sel=window.getSelection();"
        "    if(!sel || !sel.rangeCount) return false;"
        "    if(document.queryCommandSupported && document.queryCommandSupported('insertText')){"
        "      if(replaceBefore>0){"
        "        for(var i=0;i<replaceBefore;i++) document.execCommand('delete', false, null);"
        "      }"
        "      if(t.length) document.execCommand('insertText', false, t);"
        "      return true;"
        "    }"
        "    var r=sel.getRangeAt(0);"
        "    if(r.collapsed && replaceBefore>0){"
        "      var available = Math.min(replaceBefore, r.startOffset);"
        "      if(available>0) r.setStart(r.startContainer, r.startOffset-available);"
        "    }"
        "    r.deleteContents();"
        "    if(t.length){"
        "      var node=document.createTextNode(t);"
        "      r.insertNode(node);"
        "      r.setStartAfter(node);"
        "    }"
        "    r.collapse(true);"
        "    sel.removeAllRanges();"
        "    sel.addRange(r);"
        "    fireInput(el,t, t.length ? 'insertText' : 'deleteContentBackward');"
        "    return true;"
        "  }"
        "  var start=el.selectionStart;"
        "  var end=el.selectionEnd;"
        "  if(start===end && replaceBefore>0) start=Math.max(0,start-replaceBefore);"
        "  if(typeof start==='number' && typeof end==='number' && typeof el.setRangeText==='function'){"
        "    el.setRangeText(t,start,end,'end');"
        "  } else if(typeof start==='number' && typeof end==='number'){"
        "    var v=el.value||'';"
        "    el.value=v.slice(0,start)+t+v.slice(end);"
        "    var p=start+t.length;"
        "    if(el.setSelectionRange) el.setSelectionRange(p,p);"
        "  } else {"
        "    var vv=(el.value||'');"
        "    if(replaceBefore>0 && vv.length) vv=vv.slice(0, Math.max(0, vv.length-replaceBefore));"
        "    el.value=vv+t;"
        "  }"
        "  fireInput(el,t, t.length ? 'insertText' : 'deleteContentBackward');"
        "  return true;"
        "})(%1);")
        .arg(args);
    page->runJavaScript(script);
    if (QQuickWindow* win = page->window())
        win->update();
    page->update();
    QTimer::singleShot(16, page, [page]() {
        if (QQuickWindow* win = page->window())
            win->update();
        page->update();
    });
    return true;
}

bool dispatchBackspaceToFocusedElement(WPEWebPage* page)
{
    if (!page)
        return false;

    page->runJavaScript(QStringLiteral(
        "(function(){"
        "  function isEditable(e){"
        "    if(!e) return false;"
        "    if(e.isContentEditable) return true;"
        "    var tag=(e.tagName||'').toLowerCase();"
        "    if(tag==='textarea') return !e.readOnly && !e.disabled;"
        "    if(tag==='input'){"
        "      var tp=(e.type||'text').toLowerCase();"
        "      var blocked={button:1,submit:1,reset:1,checkbox:1,radio:1,file:1,image:1,range:1,color:1,hidden:1};"
        "      return !blocked[tp] && !e.readOnly && !e.disabled;"
        "    }"
        "    return false;"
        "  }"
        "  function fireInput(el){"
        "    try {"
        "      if (typeof InputEvent === 'function')"
        "        el.dispatchEvent(new InputEvent('input',{bubbles:true,inputType:'deleteContentBackward'}));"
        "      else"
        "        el.dispatchEvent(new Event('input',{bubbles:true}));"
        "    } catch(_e) {"
        "      el.dispatchEvent(new Event('input',{bubbles:true}));"
        "    }"
        "  }"
        "  var el=document.activeElement;"
        "  if(!isEditable(el)) return false;"
        "  if(el.isContentEditable){"
        "    var sel=window.getSelection();"
        "    if(!sel || !sel.rangeCount) return false;"
        "    if(document.queryCommandSupported && document.queryCommandSupported('delete')){"
        "      document.execCommand('delete', false, null);"
        "      return true;"
        "    }"
        "    var r=sel.getRangeAt(0);"
        "    if(r.collapsed){"
        "      if(r.startOffset<=0) return true;"
        "      r.setStart(r.startContainer,r.startOffset-1);"
        "    }"
        "    r.deleteContents();"
        "    sel.removeAllRanges();"
        "    sel.addRange(r);"
        "    fireInput(el);"
        "    return true;"
        "  }"
        "  var start=el.selectionStart;"
        "  var end=el.selectionEnd;"
        "  var v=el.value||'';"
        "  if(typeof start==='number' && typeof end==='number'){"
        "    if(start!==end){"
        "      el.value=v.slice(0,start)+v.slice(end);"
        "    } else if(start>0){"
        "      el.value=v.slice(0,start-1)+v.slice(end);"
        "      start-=1;"
        "    }"
        "    if(el.setSelectionRange) el.setSelectionRange(start,start);"
        "  } else if(v.length){"
        "    el.value=v.slice(0,-1);"
        "  }"
        "  fireInput(el);"
        "  return true;"
        "})();"));
    if (QQuickWindow* win = page->window())
        win->update();
    page->update();
    QTimer::singleShot(16, page, [page]() {
        if (QQuickWindow* win = page->window())
            win->update();
        page->update();
    });
    return true;
}

bool startSelectionAtPoint(WPEWebPage* page, qreal x, qreal y)
{
    if (!page)
        return false;

    const QString script = QStringLiteral(
        "(function(x,y){"
        "  function pointRange(){"
        "    if (document.caretRangeFromPoint)"
        "      return document.caretRangeFromPoint(x,y);"
        "    if (document.caretPositionFromPoint) {"
        "      var p = document.caretPositionFromPoint(x,y);"
        "      if (p) {"
        "        var r = document.createRange();"
        "        r.setStart(p.offsetNode, p.offset);"
        "        r.collapse(true);"
        "        return r;"
        "      }"
        "    }"
        "    return null;"
        "  }"
        "  function isWordChar(ch) { return /\\S/.test(ch); }"
        "  var sel = window.getSelection && window.getSelection();"
        "  if (!sel) return false;"
        "  var range = pointRange();"
        "  if (!range) return false;"
        "  var node = range.startContainer;"
        "  var offset = range.startOffset;"
        "  if (!node) return false;"
        "  if (node.nodeType !== Node.TEXT_NODE) {"
        "    if (node.childNodes && node.childNodes.length) {"
        "      if (offset >= node.childNodes.length) offset = node.childNodes.length - 1;"
        "      if (offset < 0) offset = 0;"
        "      var child = node.childNodes[offset];"
        "      if (child && child.nodeType === Node.TEXT_NODE) {"
        "        node = child;"
        "        offset = 0;"
        "      }"
        "    }"
        "  }"
        "  if (node.nodeType !== Node.TEXT_NODE) return false;"
        "  var text = node.data || '';"
        "  if (!text.length) return false;"
        "  var start = offset;"
        "  var end = offset;"
        "  while (start > 0 && isWordChar(text.charAt(start - 1))) start--;"
        "  while (end < text.length && isWordChar(text.charAt(end))) end++;"
        "  if (start === end) {"
        "    if (end < text.length) end++;"
        "    else if (start > 0) start--;"
        "  }"
        "  try {"
        "    var wordRange = document.createRange();"
        "    wordRange.setStart(node, start);"
        "    wordRange.setEnd(node, end);"
        "    sel.removeAllRanges();"
        "    sel.addRange(wordRange);"
        "    return true;"
        "  } catch (e) {"
        "    return false;"
        "  }"
        "})(%1,%2);")
        .arg(x, 0, 'f', 2)
        .arg(y, 0, 'f', 2);

    page->runJavaScript(script);
    if (QQuickWindow* win = page->window())
        win->update();
    page->update();
    QTimer::singleShot(16, page, [page]() {
        if (QQuickWindow* win = page->window())
            win->update();
        page->update();
    });
    return true;
}

bool shouldInterceptSoftKeyboardEvent(const QKeyEvent* event)
{
    if (!event)
        return false;
    QInputMethod* inputMethod = QGuiApplication::inputMethod();
    if (!inputMethod || !inputMethod->isVisible())
        return false;
    return !event->text().isEmpty() || event->key() == Qt::Key_Backspace;
}

} // namespace

WPEWebPage::WPEWebPage(QQuickItem *parent)
    : WPEQtView(parent)
    , m_security(new WPESecurityInfo(this))
{
    setFlag(QQuickItem::ItemAcceptsInputMethod, true);

    m_framePump.setInterval(33);
    m_framePump.setTimerType(Qt::PreciseTimer);
    connect(&m_framePump, &QTimer::timeout, this, [this]() {
        if (isVisible()) {
            if (QQuickWindow *w = window()) {
                w->update();
            }
            update();
        }
    });

    // Set a credible mobile Chrome UA from the start
    setUserAgent(QStringLiteral(
        "Mozilla/5.0 (Linux; Android 13; Pixel 7) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/131.0.0.0 Mobile Safari/537.36"));

    connect(this, &WPEQtView::loadingChanged,
            this, &WPEWebPage::onLoadingChanged);
    connect(this, &WPEQtView::scrollPositionChanged,
            this, [this](qreal scrollY, qreal scrollHeight, qreal innerHeight) {
        bool atTop = scrollY <= 0;
        bool atBottom = (scrollHeight - scrollY - innerHeight) <= 1.0;
        if (atTop != m_atYBeginning) {
            m_atYBeginning = atTop;
            emit atYBeginningChanged();
        }
        if (atBottom != m_atYEnd) {
            m_atYEnd = atBottom;
            emit atYEndChanged();
        }
        // Chrome gesture: hide toolbar when scrolling down, show when scrolling up
        if (m_chromeGestureEnabled && !m_fixedToolbar && !m_forcedChrome) {
            qreal delta = scrollY - m_lastScrollY;
            if (atTop) {
                setChrome(true);
            } else if (delta > m_chromeGestureThreshold) {
                setChrome(false);
            } else if (delta < -m_chromeGestureThreshold) {
                setChrome(true);
            }
        }
        m_lastScrollY = scrollY;
    });
    connect(this, &WPEQtView::faviconUrlChanged,
            this, &WPEWebPage::setFavicon);
    connect(this, &WPEQtView::selectedTextChanged,
            this, [this](const QString& text) {
        bool wasActive = m_textSelectionActive;
        m_selectedText = text;
        m_textSelectionActive = !text.isEmpty();
        Q_EMIT selectionTextChanged();
        if (wasActive != m_textSelectionActive)
            Q_EMIT textSelectionActiveChanged();
        // Clear handle positions when selection is cleared
        if (!m_textSelectionActive) {
            m_selectionStartX = m_selectionStartY = m_selectionEndX = m_selectionEndY = 0.0;
            Q_EMIT selectionHandlesUpdated();
        }
    });

    connect(this, &WPEQtView::selectionHandlesChanged,
            this, [this](qreal sx, qreal sy, qreal ex, qreal ey) {
        m_selectionStartX = sx;
        m_selectionStartY = sy;
        m_selectionEndX = ex;
        m_selectionEndY = ey;
        Q_EMIT selectionHandlesUpdated();
    });

    connect(this, &WPEQtView::enterFullscreenRequested,
            this, [this]() {
        if (!m_fullscreen) {
            m_fullscreen = true;
            emit fullscreenChanged();
        }
    });
    connect(this, &WPEQtView::leaveFullscreenRequested,
            this, [this]() {
        if (m_fullscreen) {
            m_fullscreen = false;
            emit fullscreenChanged();
        }
    });
    connect(this, &WPEQtView::webViewCreated, this, [this]() {
        if (WebKitWebView* wv = webView()) {
            g_signal_connect(wv, "decide-policy", G_CALLBACK(onDecidePolicy), nullptr);
            g_signal_connect(
                wv, "run-file-chooser",
                G_CALLBACK(+[](WebKitWebView*, WebKitFileChooserRequest* request, gpointer userData) -> gboolean {
                    auto *page = static_cast<WPEWebPage*>(userData);
                    return page && page->handleFileChooserRequest(request);
                }),
                this);

            // --- HTML <select> via JS bridge ---
            WebKitUserContentManager* ucm = webkit_web_view_get_user_content_manager(wv);
            // Connect BEFORE registering (per documentation, to avoid race conditions)
            g_signal_connect(ucm, "script-message-received::selectBridge",
                             G_CALLBACK(onSelectBridgeMessage), this);
            gboolean regOk = webkit_user_content_manager_register_script_message_handler(ucm, "selectBridge", nullptr);
            fprintf(stderr, "[WPE-SELECT] register_script_message_handler returned %d, ucm=%p\n", (int)regOk, ucm);

            // Inject JS: intercept <select> taps via multiple event types
            static const gchar* selectBridgeJs = R"JS(
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
    document.addEventListener('touchstart',  handleSelectActivation, true);
    document.addEventListener('pointerdown', handleSelectActivation, true);
    document.addEventListener('click',       handleSelectActivation, true);
    console.error('[WPE-SELECT-JS] event listeners registered');
})();
)JS";
            WebKitUserScript* script = webkit_user_script_new(
                selectBridgeJs,
                WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                nullptr, nullptr);
            webkit_user_content_manager_add_script(ucm, script);
            webkit_user_script_unref(script);

            onSelectionBridgeInstall(ucm, this);

            WebKitNetworkSession* session = webkit_web_view_get_network_session(wv);
            if (!session) {
                session = webkit_network_session_get_default();
            }
            WebKitCookieManager* cookieManager = session ? webkit_network_session_get_cookie_manager(session) : nullptr;
            if (cookieManager) {
                const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
                const QString cookieDir = dataDir.isEmpty()
                    ? QStringLiteral("/home/defaultuser/.local/share/org.sailfishos/browser")
                    : dataDir;
                QDir().mkpath(cookieDir);
                const QString cookieFile = cookieDir + QStringLiteral("/cookies.sqlite");
                webkit_cookie_manager_set_persistent_storage(
                    cookieManager,
                    cookieFile.toUtf8().constData(),
                    WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
                webkit_cookie_manager_set_accept_policy(cookieManager, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
            }
        }
    });

    updateFramePumpState();
}

WPEWebPage::~WPEWebPage()
{
    clearFileChooserRequest(true);
}

int WPEWebPage::tabId() const { return m_tabId; }

void WPEWebPage::setTabId(int tabId)
{
    if (m_tabId != tabId) {
        m_tabId = tabId;
        emit tabIdChanged();
    }
}

bool WPEWebPage::painted() const { return m_painted; }
bool WPEWebPage::domContentLoaded() const { return m_domContentLoaded; }

bool WPEWebPage::chrome() const { return m_chrome; }

void WPEWebPage::setChrome(bool chrome)
{
    if (m_chrome != chrome) {
        m_chrome = chrome;
        emit chromeChanged();
    }
}

bool WPEWebPage::forcedChrome() const { return m_forcedChrome; }

void WPEWebPage::setForcedChrome(bool forcedChrome)
{
    if (m_forcedChrome != forcedChrome) {
        m_forcedChrome = forcedChrome;
        emit forcedChromeChanged();
    }
}

bool WPEWebPage::fullscreen() const { return m_fullscreen; }
bool WPEWebPage::atYBeginning() const { return m_atYBeginning; }
bool WPEWebPage::atYEnd() const { return m_atYEnd; }

QString WPEWebPage::favicon() const { return m_favicon; }

void WPEWebPage::setFavicon(const QString &favicon)
{
    if (m_favicon != favicon) {
        m_favicon = favicon;
        emit faviconChanged();
    }
}

qreal WPEWebPage::fullscreenHeight() const { return m_fullscreenHeight; }

void WPEWebPage::setFullscreenHeight(qreal height)
{
    if (!qFuzzyCompare(m_fullscreenHeight, height)) {
        m_fullscreenHeight = height;
        emit fullscreenHeightChanged();
    }
}

qreal WPEWebPage::toolbarHeight() const { return m_toolbarHeight; }

void WPEWebPage::setToolbarHeight(qreal height)
{
    if (!qFuzzyCompare(m_toolbarHeight, height)) {
        m_toolbarHeight = height;
        emit toolbarHeightChanged();
    }
}

bool WPEWebPage::active() const { return m_active; }

void WPEWebPage::setActive(bool active)
{
    if (m_active != active) {
        m_active = active;
        setVisible(active);
        updateFramePumpState();
        emit activeChanged();
    }
}

bool WPEWebPage::throttlePainting() const { return m_throttlePainting; }

void WPEWebPage::setThrottlePainting(bool throttle)
{
    if (m_throttlePainting != throttle) {
        m_throttlePainting = throttle;
        updateFramePumpState();
        emit throttlePaintingChanged();
    }
}

bool WPEWebPage::chromeGestureEnabled() const { return m_chromeGestureEnabled; }

void WPEWebPage::setChromeGestureEnabled(bool enabled)
{
    if (m_chromeGestureEnabled != enabled) {
        m_chromeGestureEnabled = enabled;
        emit chromeGestureEnabledChanged();
    }
}

qreal WPEWebPage::chromeGestureThreshold() const { return m_chromeGestureThreshold; }

void WPEWebPage::setChromeGestureThreshold(qreal threshold)
{
    if (!qFuzzyCompare(m_chromeGestureThreshold, threshold)) {
        m_chromeGestureThreshold = threshold;
        emit chromeGestureThresholdChanged();
    }
}

bool WPEWebPage::fixedToolbar() const { return m_fixedToolbar; }

void WPEWebPage::setFixedToolbar(bool fixed)
{
    if (m_fixedToolbar != fixed) {
        m_fixedToolbar = fixed;
        emit fixedToolbarChanged();
    }
}

bool WPEWebPage::loaded() const { return m_loaded; }
bool WPEWebPage::moving() const { return false; }

QVariant WPEWebPage::resurrectedContentRect() const { return m_resurrectedContentRect; }

void WPEWebPage::setResurrectedContentRect(const QVariant &rect)
{
    m_resurrectedContentRect = rect;
    emit resurrectedContentRectChanged();
}

bool WPEWebPage::dragging() const { return false; }

bool WPEWebPage::desktopMode() const { return m_desktopMode; }

void WPEWebPage::setDesktopMode(bool desktop)
{
    const bool changed = (m_desktopMode != desktop);
    m_desktopMode = desktop;
    static const char* mobileUA =
        "Mozilla/5.0 (Linux; Android 13; Pixel 7) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/131.0.0.0 Mobile Safari/537.36";
    static const char* desktopUA =
        "Mozilla/5.0 (X11; Linux x86_64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/131.0.0.0 Safari/537.36";
    setUserAgent(QString::fromUtf8(desktop ? desktopUA : mobileUA));
    if (changed) {
        emit desktopModeChanged();
        // Reload so the server sends the correct page variant.
        setUrl(url());
    }
}

void WPEWebPage::applyInitialDeviceScale(qreal scale)
{
    setDeviceScaleFactor(scale);
    rememberDefaultZoomLevel(scale);
}

double WPEWebPage::currentPageZoomLevel() const
{
    WebKitWebView *wv = webView();
    if (wv) {
        return webkit_web_view_get_zoom_level(wv);
    }
    if (m_defaultZoomLevelInitialized) {
        return m_defaultZoomLevel;
    }
    return 1.0;
}

void WPEWebPage::setPageZoomLevel(double zoomLevel)
{
    WebKitWebView *wv = webView();
    if (!wv) {
        return;
    }
    webkit_web_view_set_zoom_level(wv, zoomLevel);
}

void WPEWebPage::rememberDefaultZoomLevel(double zoomLevel)
{
    if (zoomLevel <= 0.0) {
        return;
    }
    m_defaultZoomLevel = zoomLevel;
    m_defaultZoomLevelInitialized = true;
}

double WPEWebPage::minimumPinchZoomLevel() const
{
    return m_defaultZoomLevelInitialized
        ? m_defaultZoomLevel * kMinimumPinchZoomFactor
        : kMinimumPinchZoomFactor;
}

double WPEWebPage::maximumPinchZoomLevel() const
{
    return m_defaultZoomLevelInitialized
        ? m_defaultZoomLevel * kMaximumPinchZoomFactor
        : kMaximumPinchZoomFactor;
}

void WPEWebPage::loadTab(const QString &url, bool force)
{
    QUrl newUrl(url);
    if (force || newUrl != this->url()) {
        setUrl(newUrl);
    }
}

void WPEWebPage::grabToFile(const QSize &size)
{
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                       + QStringLiteral("/thumbnails");
    QDir().mkpath(cacheDir);
    const QString filePath = cacheDir + QStringLiteral("/") + QString::number(m_tabId) + QStringLiteral(".png");

    // Capture at target width but full item height, then crop top to target height.
    // This avoids squishing the page content to fit the thumbnail aspect ratio.
    QSize captureSize(size.width(), qRound(height()));
    m_grabResult = grabToImage(captureSize);
    if (!m_grabResult)
        return;

    QSharedPointer<QQuickItemGrabResult> result = m_grabResult;
    connect(result.data(), &QQuickItemGrabResult::ready, this, [this, result, filePath, size]() {
        QImage img = result->image();
        if (img.height() > size.height())
            img = img.copy(0, 0, img.width(), size.height());
        if (img.save(filePath, "PNG")) {
            emit fileGrabWritten(filePath);
        }
        m_grabResult.clear();
    });
}

void WPEWebPage::grabThumbnail(const QSize &size)
{
    // Same: capture full height, crop top to target height.
    QSize captureSize(size.width(), qRound(height()));
    m_thumbnailResult = grabToImage(captureSize);
    if (!m_thumbnailResult)
        return;

    QSharedPointer<QQuickItemGrabResult> result = m_thumbnailResult;
    connect(result.data(), &QQuickItemGrabResult::ready, this, [this, result, size]() {
        QImage img = result->image();
        if (img.height() > size.height())
            img = img.copy(0, 0, img.width(), size.height());
        QByteArray ba;
        QBuffer buf(&ba);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        buf.close();
        emit thumbnailResult(QString::fromLatin1(ba.toBase64()));
        m_thumbnailResult.clear();
    });
}

void WPEWebPage::forceChrome(bool forced)
{
    setForcedChrome(forced);
    if (forced) {
        setChrome(true);
    }
}

void WPEWebPage::suspendView()
{
    setVisible(false);
    updateFramePumpState();
}

void WPEWebPage::resumeView()
{
    setVisible(true);
    updateFramePumpState();
    update();
}

void WPEWebPage::sendAsyncMessage(const QString &name, const QVariant &data)
{
    if (name == QStringLiteral("Browser:SelectionStart")) {
        const QVariantMap map = data.toMap();
        const qreal x = map.value(QStringLiteral("xPos")).toReal();
        const qreal y = map.value(QStringLiteral("yPos")).toReal();
        startSelectionAtPoint(this, x, y);
    }
}

void WPEWebPage::addMessageListener(const QString &)
{
    // stub
}

void WPEWebPage::clearSelection()
{
    handleJsSelectionClear();
    runJavaScript(QStringLiteral("window.getSelection().removeAllRanges();"));
}

void WPEWebPage::selectAll()
{
    runJavaScript(QStringLiteral("document.execCommand('selectAll');"));
}

void WPEWebPage::copyToClipboard()
{
    if (!m_selectedText.isEmpty())
        QGuiApplication::clipboard()->setText(m_selectedText);
}

void WPEWebPage::handleJsSelectionClear()
{
    const bool wasActive = m_textSelectionActive;
    const bool hadSelection = !m_selectedText.isEmpty() || m_selectionStartX > 0.0 || m_selectionEndX > 0.0;

    m_selectedText.clear();
    m_textSelectionActive = false;
    m_selectionStartX = m_selectionStartY = m_selectionEndX = m_selectionEndY = 0.0;

    if (hadSelection)
        Q_EMIT selectionTextChanged();
    if (wasActive)
        Q_EMIT textSelectionActiveChanged();
    if (hadSelection || wasActive)
        Q_EMIT selectionHandlesUpdated();
}

void WPEWebPage::handleJsSelectionUpdate(const QString &text, qreal startX, qreal startY, qreal endX, qreal endY)
{
    const qreal zoom = currentPageZoomLevel();
    const bool wasActive = m_textSelectionActive;
    const bool textChanged = (m_selectedText != text);
    const bool activeNow = !text.isEmpty();
    const bool handlesChanged = !qFuzzyCompare(m_selectionStartX, startX * zoom)
        || !qFuzzyCompare(m_selectionStartY, startY * zoom)
        || !qFuzzyCompare(m_selectionEndX, endX * zoom)
        || !qFuzzyCompare(m_selectionEndY, endY * zoom);

    m_selectedText = text;
    m_textSelectionActive = activeNow;
    m_selectionStartX = startX * zoom;
    m_selectionStartY = startY * zoom;
    m_selectionEndX = endX * zoom;
    m_selectionEndY = endY * zoom;

    if (textChanged)
        Q_EMIT selectionTextChanged();
    if (wasActive != activeNow)
        Q_EMIT textSelectionActiveChanged();
    if (textChanged || handlesChanged || wasActive != activeNow)
        Q_EMIT selectionHandlesUpdated();

    QVariantMap data;
    data.insert(QStringLiteral("text"), text);
    data.insert(QStringLiteral("startX"), startX);
    data.insert(QStringLiteral("startY"), startY);
    data.insert(QStringLiteral("endX"), endX);
    data.insert(QStringLiteral("endY"), endY);
    data.insert(QStringLiteral("cursorX"), endX);
    data.insert(QStringLiteral("cursorY"), endY);
    data.insert(QStringLiteral("sx"), startX);
    data.insert(QStringLiteral("sy"), startY);
    data.insert(QStringLiteral("ex"), endX);
    data.insert(QStringLiteral("ey"), endY);
    emit recvAsyncMessage(QStringLiteral("Content:SelectionRange"), data);
}

void WPEWebPage::moveSelectionStart(qreal cssX, qreal cssY)
{
    const qreal zoom = currentPageZoomLevel();
    if (zoom > 0.0) {
        cssX /= zoom;
        cssY /= zoom;
    }

    QString js = QStringLiteral(
        "(function(x,y){"
        "  var sel=window.getSelection();"
        "  if(!sel.rangeCount)return;"
        "  var range=sel.getRangeAt(0);"
        "  var r=document.caretRangeFromPoint(x,y);"
        "  if(!r)return;"
        "  try{range.setStart(r.startContainer,r.startOffset);"
        "  sel.removeAllRanges();sel.addRange(range);}catch(e){}"
        "})(%1,%2);"
    ).arg(cssX, 0, 'f', 2).arg(cssY, 0, 'f', 2);
    runJavaScript(js);
}

void WPEWebPage::moveSelectionEnd(qreal cssX, qreal cssY)
{
    const qreal zoom = currentPageZoomLevel();
    if (zoom > 0.0) {
        cssX /= zoom;
        cssY /= zoom;
    }

    QString js = QStringLiteral(
        "(function(x,y){"
        "  var sel=window.getSelection();"
        "  if(!sel.rangeCount)return;"
        "  var range=sel.getRangeAt(0);"
        "  var r=document.caretRangeFromPoint(x,y);"
        "  if(!r)return;"
        "  try{range.setEnd(r.startContainer,r.startOffset);"
        "  sel.removeAllRanges();sel.addRange(range);}catch(e){}"
        "})(%1,%2);"
    ).arg(cssX, 0, 'f', 2).arg(cssY, 0, 'f', 2);
    runJavaScript(js);
}

bool WPEWebPage::textSelectionActive() const { return m_textSelectionActive; }

QObject* WPEWebPage::textSelectionController() { return this; }

QString WPEWebPage::selectedText() const { return m_selectedText; }

bool WPEWebPage::isPhoneNumber() const
{
    static const QRegularExpression re(QStringLiteral("^[+\\d][\\d\\s\\-().+]{6,}$"));
    return re.match(m_selectedText.trimmed()).hasMatch();
}

QString WPEWebPage::searchUri() const
{
    return QStringLiteral("https://duckduckgo.com/?q=") +
           QString::fromUtf8(QUrl::toPercentEncoding(m_selectedText.trimmed()));
}

void WPEWebPage::onLoadingChanged(WPEQtViewLoadRequest *loadRequest)
{
    if (!loadRequest)
        return;

    switch (loadRequest->status()) {
    case WPEQtView::LoadStartedStatus:
        clearFileChooserRequest(true);
        m_domContentLoaded = false;
        m_loaded = false;
        m_favicon.clear();
        m_lastScrollY = 0.0;
        m_atYBeginning = true;
        m_atYEnd = false;
        if (m_textSelectionActive) {
            m_selectedText.clear();
            m_textSelectionActive = false;
            Q_EMIT selectionTextChanged();
            Q_EMIT textSelectionActiveChanged();
        }
        // Reset security on new navigation
        static_cast<WPESecurityInfo*>(m_security)->reset();
        emit securityChanged();
        // Reset visual pinch zoom so new page starts at 1:1
        if (!qFuzzyCompare(m_visualScale, 1.0)) {
            m_visualScale = 1.0;
        }
        setChrome(true);
        emit domContentLoadedChanged();
        emit loadedChanged();
        emit faviconChanged();
        break;

    case WPEQtView::LoadSucceededStatus:
        m_domContentLoaded = true;
        m_loaded = true;
        updateSecurityInfo();
        emit domContentLoadedChanged();
        emit loadedChanged();
        break;

    case WPEQtView::LoadStoppedStatus:
    case WPEQtView::LoadFailedStatus:
        m_loaded = false;
        emit loadedChanged();
        break;
    }
}

bool WPEWebPage::handleFileChooserRequest(WebKitFileChooserRequest *request)
{
    if (!request) {
        return false;
    }

    clearFileChooserRequest(true);

    m_fileChooserRequest = WEBKIT_FILE_CHOOSER_REQUEST(g_object_ref(request));

    const bool selectMultiple = webkit_file_chooser_request_get_select_multiple(request);
    const gchar* const* mimeTypes = webkit_file_chooser_request_get_mime_types(request);
    QStringList requestedMimeTypes;
    if (mimeTypes) {
        for (guint i = 0; mimeTypes[i]; ++i) {
            requestedMimeTypes.append(QString::fromUtf8(mimeTypes[i]));
        }
    }
    fprintf(stderr, "[WPE-FILE] open request=%p selectMultiple=%d mimeTypes=%s\n",
            static_cast<void*>(request), selectMultiple ? 1 : 0,
            requestedMimeTypes.join(QStringLiteral(",")).toUtf8().constData());
    if (m_fileChooserSelectMultiple != selectMultiple) {
        m_fileChooserSelectMultiple = selectMultiple;
        emit fileChooserSelectMultipleChanged();
    }

    const QStringList filters = requestNameFilters(request);
    if (m_fileChooserNameFilters != filters) {
        m_fileChooserNameFilters = filters;
        emit fileChooserNameFiltersChanged();
    }

    if (!m_fileChooserActive) {
        m_fileChooserActive = true;
        emit fileChooserActiveChanged();
    }

    return true;
}

void WPEWebPage::clearFileChooserRequest(bool cancelRequest)
{
    fprintf(stderr, "[WPE-FILE] clear request=%p cancel=%d active=%d\n",
            static_cast<void*>(m_fileChooserRequest), cancelRequest ? 1 : 0, m_fileChooserActive ? 1 : 0);
    if (m_fileChooserRequest) {
        if (cancelRequest) {
            webkit_file_chooser_request_cancel(m_fileChooserRequest);
        }
        g_object_unref(m_fileChooserRequest);
        m_fileChooserRequest = nullptr;
    }

    if (m_fileChooserActive) {
        m_fileChooserActive = false;
        emit fileChooserActiveChanged();
    }
    if (!m_fileChooserNameFilters.isEmpty()) {
        m_fileChooserNameFilters.clear();
        emit fileChooserNameFiltersChanged();
    }
    if (m_fileChooserSelectMultiple) {
        m_fileChooserSelectMultiple = false;
        emit fileChooserSelectMultipleChanged();
    }
}

void WPEWebPage::itemChange(ItemChange change, const ItemChangeData &value)
{
    WPEQtView::itemChange(change, value);
    if (change == ItemSceneChange && value.window) {
        connect(value.window, &QQuickWindow::frameSwapped,
                this, &WPEWebPage::onFrameSwapped,
                Qt::UniqueConnection);
    } else if (change == ItemVisibleHasChanged) {
        updateFramePumpState();
    }
}

void WPEWebPage::updateFramePumpState()
{
    const bool shouldPump = isVisible();
    if (shouldPump) {
        if (!m_framePump.isActive()) {
            m_framePump.start();
        }
    } else if (m_framePump.isActive()) {
        m_framePump.stop();
    }
}

QVariant WPEWebPage::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImEnabled)
        return true;
    return QQuickItem::inputMethodQuery(query);
}

void WPEWebPage::inputMethodEvent(QInputMethodEvent *event)
{
    if (!event)
        return;

    bool handled = false;
    const QString committed = event->commitString();
    const QString preedit = event->preeditString();
    if (!committed.isEmpty()) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const bool duplicateOfRecentSoftKey =
            (committed == m_lastSoftKeyboardText) &&
            (nowMs - m_lastSoftKeyboardTextTimeMs >= 0) &&
            (nowMs - m_lastSoftKeyboardTextTimeMs < 200);

        if (!duplicateOfRecentSoftKey) {
            const int replaceBefore = m_lastPreeditText.size();
            handled = dispatchTextToFocusedElement(this, committed, replaceBefore) || handled;
        }

        m_lastSoftKeyboardText.clear();
        m_lastSoftKeyboardTextTimeMs = 0;
        m_lastPreeditText.clear();
    } else if (!preedit.isEmpty()) {
        handled = dispatchTextToFocusedElement(this, preedit, m_lastPreeditText.size()) || handled;
        m_lastPreeditText = preedit;
    } else if (!m_lastPreeditText.isEmpty()) {
        handled = dispatchTextToFocusedElement(this, QString(), m_lastPreeditText.size()) || handled;
        m_lastPreeditText.clear();
    }

    const int deleteCount = event->replacementLength();
    if (deleteCount > 0) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const bool duplicateOfRecentSoftBackspace =
            (nowMs - m_lastSoftBackspaceTimeMs >= 0) &&
            (nowMs - m_lastSoftBackspaceTimeMs < 200);
        if (!duplicateOfRecentSoftBackspace) {
            for (int i = 0; i < deleteCount; ++i) {
                handled = dispatchBackspaceToFocusedElement(this) || handled;
            }
        }
        m_lastSoftBackspaceTimeMs = 0;
    }

    if (handled) {
        event->accept();
        return;
    }

    QQuickItem::inputMethodEvent(event);
}

void WPEWebPage::keyPressEvent(QKeyEvent *event)
{
    if (!event) {
        return;
    }

    if (shouldInterceptSoftKeyboardEvent(event)) {
        if (event->key() == Qt::Key_Backspace) {
            dispatchBackspaceToFocusedElement(this);
            m_lastSoftBackspaceTimeMs = QDateTime::currentMSecsSinceEpoch();
        } else if (!event->text().isEmpty()) {
            dispatchTextToFocusedElement(this, event->text(), 0);
            m_lastSoftKeyboardText = event->text();
            m_lastSoftKeyboardTextTimeMs = QDateTime::currentMSecsSinceEpoch();
            m_lastPreeditText.clear();
        }
        event->accept();
        return;
    }

    WPEQtView::keyPressEvent(event);
}

void WPEWebPage::keyReleaseEvent(QKeyEvent *event)
{
    if (!event) {
        return;
    }

    if (shouldInterceptSoftKeyboardEvent(event)) {
        event->accept();
        return;
    }

    WPEQtView::keyReleaseEvent(event);
}

void WPEWebPage::mouseReleaseEvent(QMouseEvent *event)
{
    if (event) {
        m_lastInteractionX = event->localPos().x();
        m_lastInteractionY = event->localPos().y();
    }
    WPEQtView::mouseReleaseEvent(event);
    scheduleVirtualKeyboardSync();
}

void WPEWebPage::touchEvent(QTouchEvent *event)
{
    if (!event) {
        return;
    }

    forceActiveFocus();

    const QList<QTouchEvent::TouchPoint> activePoints = mergeTrackedTouchPoints(
        m_trackedTouchPoints,
        event->touchPoints(),
        event->type());
    const bool shouldSyncKeyboard = (event->type() == QEvent::TouchEnd && activePoints.size() <= 1);
    if (activePoints.size() >= 2) {
        const qreal pinchDistance = QLineF(activePoints.at(0).pos(), activePoints.at(1).pos()).length();
        WebKitWebView *wv = webView();
        if (wv && pinchDistance > 0.0) {
            if (!m_pinchZoomActive) {
                m_pinchZoomActive = true;
                m_pinchStartDistance = pinchDistance;
                m_pinchStartVisualScale = m_visualScale;

                QTouchEvent endEvent(QEvent::TouchEnd,
                                     event->device(),
                                     event->modifiers(),
                                     event->touchPointStates(),
                                     event->touchPoints());
                WPEQtView::touchEvent(&endEvent);
            } else if (m_pinchStartDistance > 0.0) {
                const qreal ratio = pinchDistance / m_pinchStartDistance;
                const qreal newVisualScale = std::clamp(
                    m_pinchStartVisualScale * ratio,
                    static_cast<qreal>(kMinimumPinchZoomFactor),
                    static_cast<qreal>(kMaximumPinchZoomFactor));

                if (!qFuzzyCompare(newVisualScale, m_visualScale)) {
                    m_visualScale = newVisualScale;

                    // Pinch center as percentage of item dimensions for CSS transform-origin
                    const QPointF center = (activePoints.at(0).pos() + activePoints.at(1).pos()) / 2.0;
                    const double cx_pct = (width()  > 0) ? center.x() / width()  * 100.0 : 50.0;
                    const double cy_pct = (height() > 0) ? center.y() / height() * 100.0 : 50.0;

                    // CSS transform: GPU-composited by WebKit — no layout reflow, no layout shift
                    char js[256];
                    snprintf(js, sizeof(js),
                        "document.documentElement.style.transform='scale(%.4f)';"
                        "document.documentElement.style.transformOrigin='%.1f%% %.1f%%';",
                        (double)m_visualScale, cx_pct, cy_pct);
                    webkit_web_view_evaluate_javascript(wv, js, -1, nullptr, nullptr, nullptr, nullptr, nullptr);
                }
            }
        }

        event->accept();
        return;
    }

    if (m_pinchZoomActive) {
        if (activePoints.size() < 2 || event->type() == QEvent::TouchEnd || event->type() == QEvent::TouchCancel) {
            // Commit pinch scale back into WebKit zoom and clear temporary CSS transform so
            // scrolling/hit-testing remain in native coordinates after the gesture ends.
            WebKitWebView *wv = webView();
            if (wv && !qFuzzyCompare(m_visualScale, 1.0)) {
                const double committedZoom = std::clamp(
                    currentPageZoomLevel() * static_cast<double>(m_visualScale),
                    minimumPinchZoomLevel(),
                    maximumPinchZoomLevel());
                setPageZoomLevel(committedZoom);

                webkit_web_view_evaluate_javascript(
                    wv,
                    "document.documentElement.style.transform='';"
                    "document.documentElement.style.transformOrigin='';",
                    -1, nullptr, nullptr, nullptr, nullptr, nullptr);
                m_visualScale = 1.0;
            }

            resetPinchZoomState(m_pinchZoomActive, m_pinchStartDistance, m_pinchStartZoomLevel);
            if (activePoints.isEmpty()) {
                m_trackedTouchPoints.clear();
            }
        } else {
            event->accept();
            return;
        }
    }

    if (activePoints.isEmpty() && (event->type() == QEvent::TouchEnd || event->type() == QEvent::TouchCancel)) {
        m_trackedTouchPoints.clear();
    }

    WPEQtView::touchEvent(event);
    if (shouldSyncKeyboard) {
        if (!event->touchPoints().isEmpty()) {
            const QTouchEvent::TouchPoint &point = event->touchPoints().constLast();
            m_lastInteractionX = point.pos().x();
            m_lastInteractionY = point.pos().y();
        }
        scheduleVirtualKeyboardSync();
    }
}

void WPEWebPage::scheduleVirtualKeyboardSync()
{
    QTimer::singleShot(0, this, [this]() {
        syncVirtualKeyboardToFocusedElement();
    });
    QTimer::singleShot(120, this, [this]() {
        syncVirtualKeyboardToFocusedElement();
    });
    QTimer::singleShot(320, this, [this]() {
        syncVirtualKeyboardToFocusedElement();
    });
}

void WPEWebPage::syncVirtualKeyboardToFocusedElement()
{
    WebKitWebView* wv = webView();
    if (!wv)
        return;

    const QString editableProbeScript = QStringLiteral(
        "(function(x,y){"
        "  function isEditable(e){"
        "    if(!e) return false;"
        "    if(e.isContentEditable) return true;"
        "    var tag=(e.tagName||'').toLowerCase();"
        "    if(tag==='textarea') return !e.readOnly && !e.disabled;"
        "    if(tag==='input'){"
        "      var t=(e.type||'text').toLowerCase();"
        "      var blocked={button:1,submit:1,reset:1,checkbox:1,radio:1,file:1,image:1,range:1,color:1,hidden:1};"
        "      return !blocked[t] && !e.readOnly && !e.disabled;"
        "    }"
        "    return false;"
        "  }"
        "  var active=document.activeElement;"
        "  if(isEditable(active)) return true;"
        "  if(!(x>=0 && y>=0)) return false;"
        "  var px=x, py=y;"
        "  var vv=window.visualViewport;"
        "  if(vv && vv.scale && vv.scale>0){"
        "    px=x/vv.scale; py=y/vv.scale;"
        "  }"
        "  var candidates=[document.elementFromPoint(px,py), document.elementFromPoint(x,y)];"
        "  for(var i=0;i<candidates.length;i++){"
        "    var e=candidates[i];"
        "    if(!e) continue;"
        "    if(e.closest){"
        "      var n=e.closest('input,textarea,[contenteditable=\"\"],[contenteditable=\"true\"]');"
        "      if(n) e=n;"
        "    }"
        "    if(isEditable(e)){"
        "      try{e.focus();}catch(_e){}"
        "      return true;"
        "    }"
        "  }"
        "  return false;"
        "})(%1,%2);")
        .arg(m_lastInteractionX, 0, 'f', 2)
        .arg(m_lastInteractionY, 0, 'f', 2);

    std::unique_ptr<KeyboardProbeData> data = std::make_unique<KeyboardProbeData>();
    data->page = this;
    webkit_web_view_evaluate_javascript(
        wv,
        editableProbeScript.toUtf8().constData(),
        -1,
        nullptr,
        nullptr,
        nullptr,
        onKeyboardProbeEvaluated,
        data.release());
}

void WPEWebPage::onFrameSwapped()
{
    if (!m_active)
        return;

    if (!m_painted) {
        m_painted = true;
        emit paintedChanged();
    }
    emit afterRendering();
}

// --- Find-in-page ---

static void onFindCountedMatches(WebKitFindController *, guint matchCount, gpointer userData)
{
    fprintf(stderr, "[WPE-FIND] countedMatches: %u\n", matchCount);
    auto *page = static_cast<WPEWebPage*>(userData);
    bool hasResult = matchCount > 0;
    if (hasResult != page->findInPageHasResult()) {
        page->setFindInPageHasResult(hasResult);
    }
}

static void onFindFoundText(WebKitFindController *, guint matchCount, gpointer userData)
{
    fprintf(stderr, "[WPE-FIND] foundText: %u\n", matchCount);
    auto *page = static_cast<WPEWebPage*>(userData);
    if (!page->findInPageHasResult()) {
        page->setFindInPageHasResult(true);
    }
}

static void onFindFailedToFindText(WebKitFindController *, gpointer userData)
{
    fprintf(stderr, "[WPE-FIND] failedToFindText\n");
    auto *page = static_cast<WPEWebPage*>(userData);
    if (page->findInPageHasResult()) {
        page->setFindInPageHasResult(false);
    }
}

void WPEWebPage::setFindInPageHasResult(bool has)
{
    fprintf(stderr, "[WPE-FIND] setFindInPageHasResult: %d -> %d\n", m_findInPageHasResult, has);
    if (m_findInPageHasResult != has) {
        m_findInPageHasResult = has;
        emit findInPageHasResultChanged();
    }
}

void WPEWebPage::findText(const QString &text, bool backwards)
{
    WebKitWebView *wv = webView();
    fprintf(stderr, "[WPE-FIND] findText called: text='%s' backwards=%d webView=%p\n",
            text.toUtf8().constData(), backwards, (void*)wv);
    if (!wv) return;

    WebKitFindController *fc = webkit_web_view_get_find_controller(wv);
    fprintf(stderr, "[WPE-FIND] findController=%p\n", (void*)fc);
    if (!fc) return;

    if (!m_findInitialized) {
        g_signal_connect(fc, "counted-matches", G_CALLBACK(onFindCountedMatches), this);
        g_signal_connect(fc, "found-text", G_CALLBACK(onFindFoundText), this);
        g_signal_connect(fc, "failed-to-find-text", G_CALLBACK(onFindFailedToFindText), this);
        m_findInitialized = true;
    }

    if (text.isEmpty()) {
        webkit_find_controller_search_finish(fc);
        setFindInPageHasResult(false);
        return;
    }

    guint32 opts = WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND;
    if (backwards)
        opts |= WEBKIT_FIND_OPTIONS_BACKWARDS;

    webkit_find_controller_search(fc, text.toUtf8().constData(), opts, G_MAXUINT);
    // Set result true immediately — GLib signals may not fire due to event loop issues.
    setFindInPageHasResult(true);
    // Keep toolbar visible during find
    forceChrome(true);
}

void WPEWebPage::findFinish()
{
    WebKitWebView *wv = webView();
    if (!wv) return;
    WebKitFindController *fc = webkit_web_view_get_find_controller(wv);
    if (fc)
        webkit_find_controller_search_finish(fc);
    setFindInPageHasResult(false);
    clearSelection();
}

// --- TLS / Security info ---

void WPEWebPage::updateSecurityInfo()
{
    auto *sec = static_cast<WPESecurityInfo*>(m_security);
    WebKitWebView *wv = webView();
    if (!wv) {
        fprintf(stderr, "[WPE-SEC] no webView\n");
        sec->reset();
        emit securityChanged();
        return;
    }

    GTlsCertificate *cert = nullptr;
    GTlsCertificateFlags flags = (GTlsCertificateFlags)0;
    gboolean hasTls = webkit_web_view_get_tls_info(wv, &cert, &flags);

    fprintf(stderr, "[WPE-SEC] hasTls=%d cert=%p flags=%u\n", hasTls, (void*)cert, (unsigned)flags);

    if (!hasTls || !cert) {
        sec->reset();
        emit securityChanged();
        return;
    }

    bool hasErrors = (flags != 0);
    QString subject, issuer, pem, notBefore, notAfter;
    QString issuerOrg, issuerCN, subjectOrg, errorDesc;

    GObjectClass *klass = G_OBJECT_GET_CLASS(cert);

    // Subject and issuer full DN strings (GLib 2.70+)
    gchar *subjectName = nullptr;
    gchar *issuerName = nullptr;
    if (g_object_class_find_property(klass, "subject-name")) {
        g_object_get(cert, "subject-name", &subjectName, NULL);
    }
    if (g_object_class_find_property(klass, "issuer-name")) {
        g_object_get(cert, "issuer-name", &issuerName, NULL);
    }
    if (subjectName) {
        subject = QString::fromUtf8(subjectName);
        g_free(subjectName);
    }
    if (issuerName) {
        issuer = QString::fromUtf8(issuerName);
        g_free(issuerName);
    }

    // Parse O= and CN= from DN strings
    auto parseDN = [](const QString &dn, const QString &key) -> QString {
        int idx = dn.indexOf(key + "=");
        if (idx < 0) return {};
        idx += key.length() + 1;
        int end = dn.indexOf(',', idx);
        return end > 0 ? dn.mid(idx, end - idx).trimmed() : dn.mid(idx).trimmed();
    };
    issuerOrg = parseDN(issuer, QStringLiteral("O"));
    issuerCN = parseDN(issuer, QStringLiteral("CN"));
    subjectOrg = parseDN(subject, QStringLiteral("O"));

    // Validity dates (GLib 2.70+)
    if (g_object_class_find_property(klass, "not-valid-before")) {
        GDateTime *dt = nullptr;
        g_object_get(cert, "not-valid-before", &dt, NULL);
        if (dt) {
            gchar *s = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S UTC");
            if (s) { notBefore = QString::fromUtf8(s); g_free(s); }
            g_date_time_unref(dt);
        }
    }
    if (g_object_class_find_property(klass, "not-valid-after")) {
        GDateTime *dt = nullptr;
        g_object_get(cert, "not-valid-after", &dt, NULL);
        if (dt) {
            gchar *s = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S UTC");
            if (s) { notAfter = QString::fromUtf8(s); g_free(s); }
            g_date_time_unref(dt);
        }
    }

    // PEM data
    gchar *pemData = nullptr;
    g_object_get(cert, "certificate-pem", &pemData, NULL);
    if (pemData) {
        pem = QString::fromUtf8(pemData);
        g_free(pemData);
    }

    // Fallback for subject if empty
    if (subject.isEmpty()) {
        const QUrl currentUrl(url());
        subject = currentUrl.host();
    }

    // Fallback for issuer from chain
    if (issuer.isEmpty()) {
        GTlsCertificate *issuerCert = g_tls_certificate_get_issuer(cert);
        if (issuerCert) {
            GObjectClass *ik = G_OBJECT_GET_CLASS(issuerCert);
            if (g_object_class_find_property(ik, "issuer-name")) {
                gchar *in = nullptr;
                g_object_get(issuerCert, "issuer-name", &in, NULL);
                if (in) { issuer = QString::fromUtf8(in); g_free(in); }
            }
            if (issuer.isEmpty()) issuer = QStringLiteral("Certificate Authority");
        }
    }

    // Error description
    if (hasErrors) {
        QStringList errors;
        if (flags & G_TLS_CERTIFICATE_UNKNOWN_CA) errors << QStringLiteral("Unknown CA");
        if (flags & G_TLS_CERTIFICATE_BAD_IDENTITY) errors << QStringLiteral("Bad identity");
        if (flags & G_TLS_CERTIFICATE_NOT_ACTIVATED) errors << QStringLiteral("Not yet valid");
        if (flags & G_TLS_CERTIFICATE_EXPIRED) errors << QStringLiteral("Expired");
        if (flags & G_TLS_CERTIFICATE_REVOKED) errors << QStringLiteral("Revoked");
        if (flags & G_TLS_CERTIFICATE_INSECURE) errors << QStringLiteral("Insecure algorithm");
        if (flags & G_TLS_CERTIFICATE_GENERIC_ERROR) errors << QStringLiteral("Generic error");
        errorDesc = errors.join(QStringLiteral(", "));
    }

    QString cipher = hasErrors ? QStringLiteral("TLS (certificate errors)") : QStringLiteral("TLS 1.2/1.3");

    fprintf(stderr, "[WPE-SEC] valid=true errors=%d subject='%s' issuer='%s' notBefore='%s' notAfter='%s'\n",
            hasErrors, subject.toUtf8().constData(), issuer.toUtf8().constData(),
            notBefore.toUtf8().constData(), notAfter.toUtf8().constData());

    sec->update(true, hasErrors, subject, issuer, cipher,
                notBefore, notAfter, pem, issuerOrg, issuerCN, subjectOrg, errorDesc);
    emit securityChanged();
}

// --- HTML <select> dropdown ---

void WPEWebPage::openSelectMenu(const QStringList &options, int selectedIndex)
{
    m_selectMenuOptions = options;
    m_selectMenuSelectedIdx = selectedIndex;
    m_selectMenuActive = true;
    emit selectMenuOptionsChanged();
    emit selectMenuSelectedIndexChanged();
    emit selectMenuActiveChanged();
}

void WPEWebPage::selectMenuOption(int index)
{
    const QString script = QStringLiteral(
        "(function(){"
        "  var el=window.__wpePendingSelect;"
        "  if(!el) return;"
        "  el.selectedIndex=%1;"
        "  el.dispatchEvent(new Event('change',{bubbles:true}));"
        "  el.dispatchEvent(new Event('input',{bubbles:true}));"
        "  window.__wpePendingSelect=null;"
        "})();"
    ).arg(index);
    runJavaScript(script);
    if (m_selectMenuActive) {
        m_selectMenuActive = false;
        emit selectMenuActiveChanged();
    }
}

void WPEWebPage::closeSelectMenu()
{
    runJavaScript(QStringLiteral("window.__wpePendingSelect=null;"));
    if (m_selectMenuActive) {
        m_selectMenuActive = false;
        emit selectMenuActiveChanged();
    }
}

void WPEWebPage::chooseFiles(const QStringList &filePaths)
{
    if (!m_fileChooserRequest) {
        fprintf(stderr, "[WPE-FILE] chooseFiles ignored (no active request)\n");
        return;
    }

    fprintf(stderr, "[WPE-FILE] chooseFiles incoming=%s\n",
            filePaths.join(QStringLiteral(" | ")).toUtf8().constData());

    QStringList normalizedPaths;
    QStringList existingPaths;
    normalizedPaths.reserve(filePaths.size());
    existingPaths.reserve(filePaths.size());
    for (const QString &entry : filePaths) {
        if (entry.isEmpty()) {
            continue;
        }
        const QUrl asUrl = QUrl::fromUserInput(entry);
        QString localPath = asUrl.isValid() && asUrl.isLocalFile() ? asUrl.toLocalFile() : entry;
        if (localPath.startsWith(QStringLiteral("home/"))) {
            localPath.prepend(QLatin1Char('/'));
        }
        localPath = QDir::cleanPath(localPath);
        if (!localPath.isEmpty()) {
            normalizedPaths.append(localPath);
            const QFileInfo fileInfo(localPath);
            if (fileInfo.exists() && fileInfo.isFile()) {
                existingPaths.append(fileInfo.absoluteFilePath());
            }
            fprintf(stderr, "[WPE-FILE] candidate localPath=%s exists=%d isFile=%d\n",
                    localPath.toUtf8().constData(), fileInfo.exists() ? 1 : 0, fileInfo.isFile() ? 1 : 0);
        }
    }

    QStringList chosenPaths = !existingPaths.isEmpty() ? existingPaths : normalizedPaths;
    if (!m_fileChooserSelectMultiple && chosenPaths.size() > 1) {
        const QString firstPath = chosenPaths.first();
        chosenPaths.clear();
        chosenPaths.append(firstPath);
    }

    if (chosenPaths.isEmpty()) {
        fprintf(stderr, "[WPE-FILE] no usable paths; cancelling chooser\n");
        clearFileChooserRequest(true);
        return;
    }

    fprintf(stderr, "[WPE-FILE] chosen=%s\n", chosenPaths.join(QStringLiteral(" | ")).toUtf8().constData());

    QVector<QByteArray> utf8Paths;
    utf8Paths.reserve(chosenPaths.size());
    QVector<const gchar*> selectedFiles;
    selectedFiles.reserve(chosenPaths.size() + 1);
    for (const QString &path : chosenPaths) {
        utf8Paths.append(path.toUtf8());
        selectedFiles.append(utf8Paths.constLast().constData());
    }
    selectedFiles.append(nullptr);

    webkit_file_chooser_request_select_files(m_fileChooserRequest, selectedFiles.constData());
    if (const gchar* const* selected = webkit_file_chooser_request_get_selected_files(m_fileChooserRequest)) {
        QStringList selectedDebug;
        for (guint i = 0; selected[i]; ++i) {
            selectedDebug.append(QString::fromUtf8(selected[i]));
        }
        fprintf(stderr, "[WPE-FILE] selected-files now=%s\n",
                selectedDebug.join(QStringLiteral(" | ")).toUtf8().constData());
    } else {
        fprintf(stderr, "[WPE-FILE] selected-files now=<none>\n");
    }
    clearFileChooserRequest(false);
}

void WPEWebPage::cancelFileChooser()
{
    fprintf(stderr, "[WPE-FILE] cancelFileChooser requested from QML\n");
    clearFileChooserRequest(true);
}
