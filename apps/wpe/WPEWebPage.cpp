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
#include <QGuiApplication>
#include <QImage>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLineF>
#include <QPointer>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>

#include "WPEQtViewLoadRequest.h"

#include <wpe/webkit.h>
#include <gio/gio.h>

namespace {

constexpr double kMinimumPinchZoomFactor = 0.5;
constexpr double kMaximumPinchZoomFactor = 3.0;

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

WPEWebPage::~WPEWebPage() = default;

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

void WPEWebPage::sendAsyncMessage(const QString &, const QVariant &)
{
    // stub: WPE handles messaging internally
}

void WPEWebPage::addMessageListener(const QString &)
{
    // stub
}

void WPEWebPage::clearSelection()
{
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

void WPEWebPage::moveSelectionStart(qreal cssX, qreal cssY)
{
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
                m_pinchStartZoomLevel = currentPageZoomLevel();
                rememberDefaultZoomLevel(m_pinchStartZoomLevel);

                QTouchEvent endEvent(QEvent::TouchEnd,
                                     event->device(),
                                     event->modifiers(),
                                     event->touchPointStates(),
                                     event->touchPoints());
                WPEQtView::touchEvent(&endEvent);
            } else if (m_pinchStartDistance > 0.0) {
                const double targetZoomLevel = std::clamp(
                    m_pinchStartZoomLevel * static_cast<double>(pinchDistance / m_pinchStartDistance),
                    minimumPinchZoomLevel(),
                    maximumPinchZoomLevel());
                if (!qFuzzyCompare(targetZoomLevel, currentPageZoomLevel())) {
                    setPageZoomLevel(targetZoomLevel);
                }
            }
        }

        event->accept();
        return;
    }

    if (m_pinchZoomActive) {
        if (activePoints.size() < 2 || event->type() == QEvent::TouchEnd || event->type() == QEvent::TouchCancel) {
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

// --- Save page as PDF ---

void WPEWebPage::savePageAsPDF(const QString &filePath)
{
    // WPE doesn't have WebKitPrintOperation or snapshot API (those are GTK-only).
    // Use JavaScript to generate a basic text dump as a workaround.
    // For a full PDF, a server-side renderer would be needed.
    Q_UNUSED(filePath);
    emit pdfSaved(filePath, false);
}
