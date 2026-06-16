/*
 * WPE WebKit engine replacement for Sailfish Browser
 * WPEWebContainer implementation
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "WPEWebContainer.h"
#include "WPEWebPage.h"
#include "WPERuntimePaths.h"
#include "AdBlockEngine.h"
#include "WPEQtViewLoadRequest.h"
#include "declarativehistorymodel.h"
#include "declarativetabmodel.h"
#include "persistenttabmodel.h"
#include "privatetabmodel.h"
#include "dbmanager.h"
#include "tab.h"

#include <QGuiApplication>
#include <QTimer>
#include <QScreen>
#include <QDebug>
#include <QFileInfo>
#include <QQuickWindow>
#include <QWindow>

#pragma push_macro("signals")
#undef signals
#include <wpe/webkit.h>
#pragma pop_macro("signals")

#include <unistd.h>

namespace {

QSizeF screenSizeOrFallback(QScreen *screen)
{
    if (screen) {
        return screen->size();
    }
    return QSizeF(WPERuntimePaths::kFallbackScreenWidth, WPERuntimePaths::kFallbackScreenHeight);
}

bool addSandboxPathIfExists(WebKitWebContext *context, const QString &path, gboolean readOnly, const QString &label)
{
    if (path.isEmpty()) {
        qDebug() << "[WPE] Sandbox path skipped (" << label << "): empty path";
        return false;
    }

    const QFileInfo pathInfo(path);
    if (!pathInfo.exists()) {
        qDebug() << "[WPE] Sandbox path skipped (" << label << "):" << path << "(missing)";
        return false;
    }

    const QByteArray encodedPath = path.toUtf8();
    webkit_web_context_add_path_to_sandbox(context, encodedPath.constData(), readOnly);
    qDebug() << "[WPE] Sandbox path enabled (" << label << "):" << path
             << (readOnly ? "[readonly]" : "[readwrite]");
    return true;
}

bool addRuntimeDirCandidate(WebKitWebContext *context, const QString &runtimeDir, const QString &source)
{
    if (runtimeDir.isEmpty()) {
        qDebug() << "[WPE] Sandbox runtime dir skipped (" << source << "): empty path";
        return false;
    }

    const QFileInfo runtimeInfo(runtimeDir);
    if (!runtimeInfo.isAbsolute()) {
        qDebug() << "[WPE] Sandbox runtime dir skipped (" << source << "):" << runtimeDir << "(not absolute)";
        return false;
    }

    if (!runtimeInfo.exists() || !runtimeInfo.isDir()) {
        qDebug() << "[WPE] Sandbox runtime dir skipped (" << source << "):" << runtimeDir << "(not available)";
        return false;
    }

    return addSandboxPathIfExists(context, runtimeDir, FALSE,
                                  QStringLiteral("runtime-dir from %1").arg(source));
}

} // namespace

WPEWebContainer::WPEWebContainer(QQuickItem *parent)
    : QQuickItem(parent)
{
    setAcceptedMouseButtons(Qt::AllButtons);
    setFiltersChildMouseEvents(false);

    // Foreground tracking follows the Wayland window visibility: lipstick
    // marks the window Minimized when the app is covered/on the home screen
    // and Hidden when the display is off. (The former QML binding via
    // fakeVisibility()/Qt.application.state evaluated false at startup and
    // never re-fired, so it cannot be trusted for page visibility.)
    connect(this, &QQuickItem::windowChanged,
            this, &WPEWebContainer::onWindowChangedForForeground);
}

void WPEWebContainer::onWindowChangedForForeground(QQuickWindow *win)
{
    if (m_foregroundWindow) {
        disconnect(m_foregroundWindow.data(), &QWindow::visibilityChanged,
                   this, &WPEWebContainer::updateForegroundFromWindow);
    }
    m_foregroundWindow = win;
    if (win) {
        connect(win, &QWindow::visibilityChanged,
                this, &WPEWebContainer::updateForegroundFromWindow);
    }
    updateForegroundFromWindow();
}

void WPEWebContainer::updateForegroundFromWindow()
{
    // Fail towards visible: only an explicit Minimized/Hidden report counts
    // as background. No window yet (startup) or an unexpected visibility
    // value must never leave the active page hidden (black content).
    const QQuickWindow *win = m_foregroundWindow.data();
    const QWindow::Visibility vis = win ? win->visibility() : QWindow::AutomaticVisibility;
    const bool fg = vis != QWindow::Minimized && vis != QWindow::Hidden;
    qDebug() << "[WPE-VIS] window visibility="
             << (win ? static_cast<int>(win->visibility()) : -1)
             << "-> foreground" << fg;
    setForeground(fg);
}

WPEWebContainer::~WPEWebContainer()
{
    qDeleteAll(m_pages);
}

static void configureNetworkProcessMemoryPressure()
{
    // Apply conservative memory pressure to the NetworkProcess before any
    // network session is created.  The NetworkProcess only handles HTTP/DNS;
    // 256 MB is already generous for mobile workloads.
    WebKitMemoryPressureSettings *s = webkit_memory_pressure_settings_new();

    // All five knobs are env-tunable for on-device A/B; the defaults below
    // (256 MB limit, conservative/strict/kill at 0.5/0.75/1.0 of it, 30 s poll)
    // apply when the corresponding variable is unset, so behaviour is unchanged
    // until something is explicitly set.
    auto envDouble = [](const char *name, double dflt) {
        bool ok = false;
        const double v = qgetenv(name).toDouble(&ok);
        return ok ? v : dflt;
    };
    bool limitOk = false;
    int memLimitMb = qEnvironmentVariableIntValue("ATLANTIC_NETPROC_MEM_LIMIT_MB", &limitOk);
    if (!limitOk || memLimitMb <= 0)
        memLimitMb = 256;                                            // MB hard ceiling
    const double conservativeFrac = envDouble("ATLANTIC_NETPROC_MEM_CONSERVATIVE", 0.5);
    const double strictFrac       = envDouble("ATLANTIC_NETPROC_MEM_STRICT", 0.75);
    const double killFrac         = envDouble("ATLANTIC_NETPROC_MEM_KILL", 1.0);
    const double pollIntervalS    = envDouble("ATLANTIC_NETPROC_MEM_POLL_S", 30.0);

    webkit_memory_pressure_settings_set_memory_limit(s, static_cast<unsigned>(memLimitMb));
    // Set thresholds high→low. Each setter validates against the CURRENT stored
    // values (WebKit defaults: conservative=0.33, strict=0.5, kill=unset):
    //   set_strict       requires  value > conservative_current  &&  value < kill_current(if set)
    //   set_kill         requires  value > strict_current
    //   set_conservative requires  value < strict_current
    // Setting conservative=0.5 FIRST tripped `g_return_if_fail(value < strictThresholdFraction)`
    // because the default strict was still 0.5 (0.5 < 0.5 is false), so the whole
    // settings object was left misconfigured. strict→kill→conservative satisfies all.
    // (Overrides that violate this ordering will be rejected by WebKit — keep
    //  conservative < strict < kill.)
    webkit_memory_pressure_settings_set_strict_threshold(s, strictFrac);
    webkit_memory_pressure_settings_set_kill_threshold(s, killFrac);
    webkit_memory_pressure_settings_set_conservative_threshold(s, conservativeFrac);
    webkit_memory_pressure_settings_set_poll_interval(s, pollIntervalS);
    webkit_network_session_set_memory_pressure_settings(s);
    webkit_memory_pressure_settings_free(s);
    qDebug() << "[WPE] NetworkProcess memory pressure: limit=" << memLimitMb << "MB"
             << "conservative=" << conservativeFrac << "strict=" << strictFrac
             << "kill=" << killFrac << "poll=" << pollIntervalS << "s";
}

void WPEWebContainer::configureSandboxPaths()
{
    // The default web context is process-global and sandbox paths cannot be
    // added once a WebProcess has spawned (g_error → abort). A second
    // WPEWebContainer instance — e.g. the render-recovery QML reload — must
    // not re-run this.
    static bool s_configured = false;
    if (s_configured) {
        qDebug() << "[WPE] Sandbox paths already configured; skipping re-run";
        return;
    }
    s_configured = true;

    // NetworkProcess memory limits must be applied before any session is created.
    configureNetworkProcessMemoryPressure();

    WebKitWebContext *ctx = webkit_web_context_get_default();
    // Cache model is env-tunable via ATLANTIC_CACHE_MODEL:
    //   "web"/"browser"  → WEB_BROWSER      (largest dead-resource + decoded-image
    //                                         caches + back/forward page cache; default)
    //   "document"       → DOCUMENT_BROWSER  (moderate caches, smaller page cache)
    //   "viewer"/"minimal" → DOCUMENT_VIEWER (no page cache, smallest caches → lowest
    //                                         WebProcess RSS; the footprint lever for the
    //                                         memory-pressure thrash on heavy feeds)
    // Default is unchanged (WEB_BROWSER) so behaviour only moves when set.
    const QByteArray cacheModelEnv = qgetenv("ATLANTIC_CACHE_MODEL").trimmed().toLower();
    WebKitCacheModel cacheModel = WEBKIT_CACHE_MODEL_WEB_BROWSER;
    const char *cacheModelName = "web_browser";
    if (cacheModelEnv == "viewer" || cacheModelEnv == "minimal") {
        cacheModel = WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER;
        cacheModelName = "document_viewer";
    } else if (cacheModelEnv == "document") {
        cacheModel = WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER;
        cacheModelName = "document_browser";
    } else if (!cacheModelEnv.isEmpty() && cacheModelEnv != "web" && cacheModelEnv != "browser") {
        qWarning() << "[WPE] Unknown ATLANTIC_CACHE_MODEL" << cacheModelEnv
                   << "- falling back to web_browser";
    }
    webkit_web_context_set_cache_model(ctx, cacheModel);
    qDebug() << "[WPE] Cache model:" << cacheModelName;

    // Register the network ad-block WebProcess extension. The Brave/Rust engine
    // only sees every subresource from inside the WebProcess (UI-process
    // decide-policy misses most), so blocking lives there. Connecting the signal
    // (rather than calling the setters once) re-applies on every WebProcess
    // launch, including respawns after a crash. The initial enabled state is
    // passed as user-data; live toggles arrive via a per-page user message.
    g_signal_connect(ctx, "initialize-web-process-extensions",
        G_CALLBACK(+[](WebKitWebContext *c, gpointer) {
            webkit_web_context_set_web_process_extensions_directory(
                c, WPERuntimePaths::kWebExtensionsDir);
            webkit_web_context_set_web_process_extensions_initialization_user_data(
                c, g_variant_new_boolean(AdBlockEngine::isEnabled()));
        }), nullptr);

    // WebProcess memory pressure: WTF Linux defaults already match target thresholds for a 4 GB device:
    //   baseThreshold = min(3 GB, ramSize()) = 3 GB
    //   conservative  = 0.33 × 3 GB ≈ 1.0 GB  ← start evicting caches
    //   strict        = 0.50 × 3 GB = 1.5 GB  ← aggressive eviction
    // No kill threshold by default; killing the WebProcess would crash all open tabs.
    // OS-level OOM handles truly runaway growth.
    addSandboxPathIfExists(ctx, QString::fromUtf8(WPERuntimePaths::kRuntimePrefix), TRUE,
                           QStringLiteral("runtime-prefix"));
    addSandboxPathIfExists(ctx, QString::fromUtf8(WPERuntimePaths::kGStreamerPluginDir), TRUE,
                           QStringLiteral("gstreamer-plugins"));
    addSandboxPathIfExists(ctx, QString::fromUtf8(WPERuntimePaths::kCompatLibDir), TRUE,
                           QStringLiteral("compat-libdir"));
    addSandboxPathIfExists(ctx, QString::fromUtf8(WPERuntimePaths::kAtlanticShareDir), TRUE,
                           QStringLiteral("atlantic-share"));
    addSandboxPathIfExists(ctx, QString::fromUtf8(WPERuntimePaths::kQtShareDir), TRUE,
                           QStringLiteral("qt-share"));
    addSandboxPathIfExists(ctx, QString::fromUtf8(WPERuntimePaths::kQtLibDir), TRUE,
                           QStringLiteral("qt-lib"));
    addSandboxPathIfExists(ctx, QString::fromUtf8(WPERuntimePaths::kSystemLibDir), TRUE,
                           QStringLiteral("system-libdir"));

    const QString runtimeDirFromEnv = QString::fromLocal8Bit(qgetenv("XDG_RUNTIME_DIR")).trimmed();
    const QString runtimeDirFromUid = QStringLiteral("/run/user/%1")
        .arg(static_cast<qulonglong>(::getuid()));
    const QString legacyRuntimeDir = QString::fromUtf8(WPERuntimePaths::kRuntimeDir);

    bool runtimeDirConfigured = addRuntimeDirCandidate(ctx, runtimeDirFromEnv, QStringLiteral("XDG_RUNTIME_DIR"));
    if (!runtimeDirConfigured) {
        runtimeDirConfigured = addRuntimeDirCandidate(ctx, runtimeDirFromUid, QStringLiteral("uid fallback"));
    }
    if (!runtimeDirConfigured && legacyRuntimeDir != runtimeDirFromUid) {
        runtimeDirConfigured = addRuntimeDirCandidate(ctx, legacyRuntimeDir, QStringLiteral("legacy fallback"));
    }
    if (!runtimeDirConfigured) {
        qWarning() << "[WPE] Sandbox runtime dir unavailable: no candidate could be enabled";
    }

    qDebug() << "[WPE] Sandbox paths configured";
}

void WPEWebContainer::trackParentSize()
{
    QQuickItem *p = parentItem();
    if (!p) {
        return;
    }

    connect(p, &QQuickItem::widthChanged, this, [this]() {
        if (parentItem() && parentItem()->width() > 0) {
            setWidth(parentItem()->width());
        }
    });
    connect(p, &QQuickItem::heightChanged, this, [this]() {
        if (parentItem() && parentItem()->height() > 0) {
            setHeight(parentItem()->height());
        }
    });
}

void WPEWebContainer::ensureContainerHasUsableSize()
{
    if (width() != 0 && height() != 0) {
        return;
    }

    const QSizeF screenSize = screenSizeOrFallback(QGuiApplication::primaryScreen());
    QQuickItem *p = parentItem();
    const qreal parentWidth = p && p->width() > 0 ? p->width() : screenSize.width();
    const qreal parentHeight = p && p->height() > 0 ? p->height() : screenSize.height();

    setWidth(parentWidth);
    setHeight(parentHeight);
    trackParentSize();
}

QSizeF WPEWebContainer::preferredPageSize(const QSizeF &screenSize) const
{
    return QSizeF(width() > 0 ? width() : screenSize.width(),
                  height() > 0 ? height() : screenSize.height());
}

qreal WPEWebContainer::initialPageDeviceScaleFactor(const QSizeF &screenSize) const
{
    // The carried-forward Qt bridge still maps device scale to page zoom here.
    // Keep that known-good behavior explicit until a true device-scale path exists.
    return screenSize.width() / WPERuntimePaths::kReferenceViewportWidth;
}

void WPEWebContainer::configurePageGeometry(WPEWebPage *page, const QSizeF &screenSize)
{
    if (!page) {
        return;
    }

    const QSizeF size = preferredPageSize(screenSize);
    page->setWidth(size.width());
    page->setHeight(size.height());
    connect(this, &QQuickItem::widthChanged, page, [this, page]() { page->setWidth(width()); });
    connect(this, &QQuickItem::heightChanged, page, [this, page]() { page->setHeight(height()); });
}

void WPEWebContainer::initializeTabModels(int nextTabId)
{
    m_persistentTabModel = new PersistentTabModel(nextTabId, nullptr);
    m_privateTabModel = new PrivateTabModel(nextTabId + 100, nullptr);

    m_tabModel = m_persistentTabModel;
    connect(m_tabModel, SIGNAL(activeTabChanged(int)), this, SLOT(onActiveTabChanged(int)));
    connect(m_tabModel, SIGNAL(tabAdded(int)), this, SLOT(onTabAdded(int)));
    connect(m_tabModel, SIGNAL(tabClosed(int)), this, SLOT(onTabClosed(int)));
    emit tabModelChanged();
}

void WPEWebContainer::restoreInitialContent()
{
    if (!m_persistentTabModel) {
        return;
    }

    qDebug() << "[WPE-INIT] persistentLoaded: initialUrl=" << m_initialUrl
             << "tabCount=" << m_persistentTabModel->count();
    if (!m_initialUrl.isEmpty()) {
        load(m_initialUrl);
        return;
    }

    if (m_persistentTabModel->count() == 0) {
        // DeclarativeTabModel::newTab() refuses about:blank when tabs are empty,
        // so create a page directly without going through the tab model.
        WPEWebPage *page = getOrCreatePage(1);
        page->setActive(true);
        page->setVisible(true);
        m_contentItem = page;
        emit contentItemChanged();
        emit needChromeChanged();
        qDebug() << "[WPE-INIT] created initial empty page directly";
        return;
    }

    const int activeId = m_persistentTabModel->activeTabId();
    if (activeId > 0) {
        activatePage(activeId);
    } else if (!m_persistentTabModel->tabs().isEmpty()) {
        activatePage(m_persistentTabModel->tabs().first().tabId());
    }
}

WPEWebPage *WPEWebContainer::contentItem() const
{
    return m_contentItem;
}

bool WPEWebContainer::needChrome() const
{
    return !m_contentItem || (m_contentItem->chrome() && !m_contentItem->fullscreen());
}

bool WPEWebContainer::selectionActive() const
{
    return m_contentItem && m_contentItem->textSelectionActive();
}

DeclarativeTabModel *WPEWebContainer::tabModel() const
{
    return m_tabModel;
}

DeclarativeTabModel *WPEWebContainer::persistentTabModel() const
{
    return m_persistentTabModel;
}

DeclarativeTabModel *WPEWebContainer::privateTabModel() const
{
    return m_privateTabModel;
}

bool WPEWebContainer::loading() const
{
    return m_contentItem ? m_contentItem->isLoading() : false;
}

int WPEWebContainer::loadProgress() const
{
    return m_contentItem ? m_contentItem->loadProgress() : 0;
}

bool WPEWebContainer::canGoForward() const
{
    return m_contentItem ? m_contentItem->canGoForward() : false;
}

bool WPEWebContainer::canGoBack() const
{
    return m_contentItem ? m_contentItem->canGoBack() : false;
}

int WPEWebContainer::tabId() const
{
    return m_tabModel ? m_tabModel->activeTabId() : 0;
}

QString WPEWebContainer::title() const
{
    if (!m_contentItem)
        return QString();

    if (m_waitingForFreshTitle)
        return m_contentItem->url().toString();

    const QString pageTitle = m_contentItem->title();
    return pageTitle.isEmpty() ? m_contentItem->url().toString() : pageTitle;
}

QString WPEWebContainer::url() const
{
    return m_contentItem ? m_contentItem->url().toString() : QString();
}

void WPEWebContainer::setForeground(bool f)
{
    if (m_foreground != f) {
        m_foreground = f;
        // Tell every page about the app-level state so WebKit can throttle
        // pages while the app is minimized (and resume them on return).
        for (auto it = m_pages.constBegin(); it != m_pages.constEnd(); ++it) {
            if (it.value()) {
                it.value()->setAppForeground(f);
            }
        }
        emit foregroundChanged();
    }
}

void WPEWebContainer::setPrivateMode(bool p)
{
    if (m_privateMode != p) {
        m_privateMode = p;
        // Disconnect old model
        if (m_tabModel) {
            disconnect(m_tabModel, nullptr, this, nullptr);
        }
        m_tabModel = p ? static_cast<DeclarativeTabModel *>(m_privateTabModel)
                       : static_cast<DeclarativeTabModel *>(m_persistentTabModel);
        if (m_tabModel) {
            connect(m_tabModel, SIGNAL(activeTabChanged(int)), this, SLOT(onActiveTabChanged(int)));
            connect(m_tabModel, SIGNAL(tabAdded(int)), this, SLOT(onTabAdded(int)));
            connect(m_tabModel, SIGNAL(tabClosed(int)), this, SLOT(onTabClosed(int)));
        }
        emit tabModelChanged();

        if (m_tabModel && m_tabModel->count() > 0) {
            const int activeId = m_tabModel->activeTabId();
            if (activeId > 0) {
                activatePage(activeId);
            } else if (!m_tabModel->tabs().isEmpty()) {
                activatePage(m_tabModel->tabs().first().tabId());
            }
        } else if (m_contentItem) {
            m_contentItem->setActive(false);
            m_contentItem->setVisible(false);
            m_contentItem = nullptr;
            emit contentItemChanged();
            emit needChromeChanged();
            emit tabIdChanged();
            emit urlChanged();
            emit titleChanged();
            emit loadingChanged();
            emit loadProgressChanged();
            emit canGoBackChanged();
            emit canGoForwardChanged();
        }

        emit privateModeChanged();
    }
}

void WPEWebContainer::setWebPageComponent(QQmlComponent *c)
{
    if (m_webPageComponent != c) {
        m_webPageComponent = c;
        emit webPageComponentChanged(c);
    }
}

void WPEWebContainer::setChromeWindow(QObject *w)
{
    if (m_chromeWindow != w) {
        m_chromeWindow = w;
        emit chromeWindowChanged();
        if (w) emit chromeExposed();
    }
}

void WPEWebContainer::setReadyToPaint(bool r)
{
    if (m_readyToPaint != r) {
        m_readyToPaint = r;
        emit readyToPaintChanged();
    }
}

void WPEWebContainer::setHistoryModel(DeclarativeHistoryModel *m)
{
    if (m_historyModel != m) {
        m_historyModel = m;
        emit historyModelChanged();
    }
}

void WPEWebContainer::classBegin()
{
}

void WPEWebContainer::componentComplete()
{
    // Configure WebKit process sandbox paths before any web view is created.
    // bubblewrap requires explicit allowlisting of paths needed by the subprocesses.
    configureSandboxPaths();

    // Qt Quick routes input events based on item bounding-box.  If the
    // container has size 0×0 (no QML anchor/size binding from the page),
    // touch events are never delivered to WPEQtView children.  Initialise
    // to screen size here; the OverlayAnimator may later animate height.
    ensureContainerHasUsableSize();

    int maxTabId = DBManager::instance()->getMaxTabId();
    int nextTabId = maxTabId + 1;

    initializeTabModels(nextTabId);

    m_completed = true;
    emit completedChanged();

    // After the persistent model finishes its async DB load, make sure something is shown.
    // If an initial URL was requested, load it; otherwise activate the persisted tab, or
    // fall back to a blank page if there are no saved tabs.
    if (m_persistentTabModel->loaded()) {
        QTimer::singleShot(0, this, &WPEWebContainer::restoreInitialContent);
    } else {
        connect(m_persistentTabModel, &DeclarativeTabModel::loadedChanged, this, &WPEWebContainer::restoreInitialContent);
    }

    if (!m_initialUrl.isEmpty())
        emit hasInitialUrlChanged();
}

void WPEWebContainer::load(const QString &url, const QString &title, bool newTab)
{
    Q_UNUSED(title)
    qDebug() << "[WPE-LOAD] load() url=" << url << "completed=" << m_completed
             << "tabModel=" << (m_tabModel ? m_tabModel->count() : -1)
             << "contentItem=" << m_contentItem
             << "newTab=" << newTab;

    if (!m_completed) {
        qDebug() << "[WPE-LOAD] not completed, saving as initialUrl";
        m_initialUrl = url;
        emit hasInitialUrlChanged();
        return;
    }

    if (!m_tabModel) { qDebug() << "[WPE-LOAD] no tabModel!"; return; }

    if (newTab || m_tabModel->count() == 0) {
        qDebug() << "[WPE-LOAD] newTab for" << url;
        m_tabModel->newTab(url, newTab);
    } else {
        if (m_contentItem) {
            const QUrl targetUrl(url);
            if (m_contentItem->url() == targetUrl) {
                qDebug() << "[WPE-LOAD] reload active contentItem:" << url;
                m_contentItem->reload();
            } else {
                qDebug() << "[WPE-LOAD] setUrl on contentItem:" << url;
                m_contentItem->setUrl(targetUrl);
            }
        } else {
            qDebug() << "[WPE-LOAD] contentItem is NULL, cannot load!";
        }
    }
}

void WPEWebContainer::reload(bool force)
{
    Q_UNUSED(force)
    if (m_contentItem) m_contentItem->reload();
}

void WPEWebContainer::goForward()
{
    if (m_contentItem) m_contentItem->goForward();
}

void WPEWebContainer::goBack()
{
    if (m_contentItem) m_contentItem->goBack();
}

int WPEWebContainer::activateTab(int tabId, const QString &url)
{
    if (!m_tabModel) return -1;
    m_tabModel->activateTabById(tabId);
    if (!url.isEmpty() && m_contentItem) {
        m_contentItem->setUrl(QUrl(url));
    }
    return tabId;
}

void WPEWebContainer::closeTab(int tabId)
{
    if (!m_tabModel) return;
    bool isActive = (tabId == this->tabId());
    m_tabModel->removeTabById(tabId, isActive);
}

void WPEWebContainer::releaseActiveTabOwnership()
{
    // stub - ownership tracking for D-Bus not needed in WPE
}

void WPEWebContainer::updateContentOrientation(Qt::ScreenOrientation orientation)
{
    emit webContentOrientationChanged(orientation);
}

void WPEWebContainer::applyContentOrientation(Qt::ScreenOrientation orientation)
{
    updateContentOrientation(orientation);
}

void WPEWebContainer::onActiveTabChanged(int activeTabId)
{
    activatePage(activeTabId);
}

void WPEWebContainer::onTabAdded(int tabId)
{
    // Pre-create the page so it's ready
    WPEWebPage *page = getOrCreatePage(tabId);
    Q_UNUSED(page)
}

void WPEWebContainer::onTabClosed(int tabId)
{
    WPEWebPage *page = m_pages.take(tabId);
    if (page) {
        page->setVisible(false);
        page->deleteLater();
        if (m_contentItem == page) {
            m_contentItem = nullptr;
            emit contentItemChanged();
            emit needChromeChanged();
        }
    }
}

void WPEWebContainer::activatePage(int tabId)
{
    qDebug() << "[WPE-ACTIVATE] tabId=" << tabId << "currentContent=" << m_contentItem;
    if (m_contentItem) {
        m_contentItem->setActive(false);
        m_contentItem->setVisible(false);
    }

    // Get or create new page
    WPEWebPage *page = getOrCreatePage(tabId);

    // Load the tab's URL if page is new (empty)
    if (m_tabModel) {
        QString tabUrl = m_tabModel->url(tabId);
        if (!tabUrl.isEmpty() && page->url().isEmpty()) {
            page->setUrl(QUrl(tabUrl));
        }
    }

    page->setActive(true);
    page->setVisible(true);

    if (m_contentItem != page) {
        m_contentItem = page;
        m_waitingForFreshTitle = page->title().isEmpty();
        emit contentItemChanged();
        emit needChromeChanged();
        emit tabIdChanged();
        emit urlChanged();
        emit titleChanged();
        emit loadingChanged();
        emit loadProgressChanged();
        emit canGoBackChanged();
        emit canGoForwardChanged();
    }
}

WPEWebPage *WPEWebContainer::getOrCreatePage(int tabId)
{
    qDebug() << "[WPE-PAGE] getOrCreatePage tabId=" << tabId << "exists=" << m_pages.contains(tabId);
    if (m_pages.contains(tabId)) {
        return m_pages[tabId];
    }

    // Create with nullptr parent so windowChanged fires AFTER WPEQtView's connect() is set up,
    // then reparent via setParentItem() to trigger configureWindow() correctly.
    WPEWebPage *page = new WPEWebPage(nullptr);
    page->setTabId(tabId);
    page->setVisible(false);
    page->setActive(false);
    page->setAppForeground(m_foreground);

    const QScreen *screen = QGuiApplication::primaryScreen();
    const QSizeF screenSize = screenSizeOrFallback(QGuiApplication::primaryScreen());
    configurePageGeometry(page, screenSize);

    // Keep the current zoom-backed device-scale bootstrap explicit here.
    if (screen) {
        page->applyInitialDeviceScale(initialPageDeviceScaleFactor(screenSize));
    }

    connectPage(page);
    m_pages[tabId] = page;

    // Reparent after all connections are set up — this fires windowChanged which triggers
    // WPEQtView::configureWindow() → createWebView().
    page->setParentItem(this);
    // Apply the current mode UA once the view exists so first navigation uses it.
    page->setDesktopMode(page->desktopMode());
    return page;
}

void WPEWebContainer::connectPage(WPEWebPage *page)
{
    // Keep tab metadata (URL/title/mode) in sync with the tab model that created this page.
    // Using the model captured at page-creation time avoids cross-updating when switching
    // between persistent/private models later.
    DeclarativeTabModel *pageModel = m_tabModel;
    if (pageModel) {
        connect(page, &WPEQtView::urlChanged, pageModel, &DeclarativeTabModel::onUrlChanged);
        connect(page, &WPEQtView::titleChanged, pageModel, &DeclarativeTabModel::onTitleChanged);
        connect(page, &WPEWebPage::desktopModeChanged, pageModel, &DeclarativeTabModel::onDesktopModeChanged);
    }

    connect(page, &WPEQtView::urlChanged, this, &WPEWebContainer::onPageUrlChanged);
    connect(page, &WPEQtView::titleChanged, this, &WPEWebContainer::onPageTitleChanged);
    // WPEQtView::loadingChanged carries a WPEQtViewLoadRequest* — use a lambda to adapt
    connect(page, &WPEQtView::loadingChanged, this, [this, page](WPEQtViewLoadRequest *) {
        if (page == m_contentItem) {
            emit loadingChanged();
            emit canGoBackChanged();
            emit canGoForwardChanged();
        }
    });
    connect(page, &WPEQtView::loadProgressChanged, this, &WPEWebContainer::onPageLoadProgressChanged);
    connect(page, &WPEWebPage::paintedChanged, this, &WPEWebContainer::onPagePaintedChanged);
    connect(page, &WPEWebPage::chromeChanged, this, &WPEWebContainer::needChromeChanged);
    connect(page, &WPEWebPage::fullscreenChanged, this, &WPEWebContainer::needChromeChanged);
    connect(page, &WPEWebPage::fileGrabWritten, this, [this, page](const QString &filePath) {
        if (m_tabModel)
            m_tabModel->updateThumbnailPath(page->tabId(), filePath);
    });
    connect(page, &WPEWebPage::crashedChanged, this, [page]() {
        if (page->crashed())
            qWarning() << "[WPE] WebProcess crashed for tab" << page->tabId()
                       << "— tab isolated, browser alive";
    });
}

void WPEWebContainer::onPageUrlChanged()
{
    WPEWebPage *page = qobject_cast<WPEWebPage *>(sender());
    if (!page || page != m_contentItem) return;

    QString newUrl = page->url().toString();
    if (newUrl == QStringLiteral("about:blank")) return;

    m_waitingForFreshTitle = true;
    emit urlChanged();
    emit titleChanged();

    // Update tab model
    if (m_tabModel) {
        // The tab model tracks URL via its own signals; here we record history
        if (m_historyModel && !newUrl.isEmpty()) {
            m_historyModel->add(newUrl, page->title());
        }
    }
}

void WPEWebContainer::onPageTitleChanged()
{
    WPEWebPage *page = qobject_cast<WPEWebPage *>(sender());
    if (!page || page != m_contentItem) return;
    m_waitingForFreshTitle = page->title().isEmpty();
    emit titleChanged();
    // Update history entry once title is known
    QString url = page->url().toString();
    QString title = page->title();
    if (m_historyModel && !url.isEmpty() && url != QStringLiteral("about:blank") && !title.isEmpty()) {
        m_historyModel->add(url, title);
    }
}

void WPEWebContainer::onPageLoadProgressChanged()
{
    WPEWebPage *page = qobject_cast<WPEWebPage *>(sender());
    if (!page || page != m_contentItem) return;
    emit loadProgressChanged();
}

void WPEWebContainer::onPagePaintedChanged()
{
    WPEWebPage *page = qobject_cast<WPEWebPage *>(sender());
    if (!page || page != m_contentItem) return;
    if (page->painted() && !m_activeTabRendered) {
        setActiveTabRendered(true);
    }
}

void WPEWebContainer::setActiveTabRendered(bool r)
{
    if (m_activeTabRendered != r) {
        m_activeTabRendered = r;
        emit activeTabRenderedChanged();
    }
}
