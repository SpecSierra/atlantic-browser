#include "browserruntime.h"

#include "browser.h"
#include "browserservice.h"
#include "declarativebookmarkmodel.h"
#include "bookmarkfiltermodel.h"
#include "desktopbookmarkwriter.h"
#include "downloadstatus.h"
#include "persistenttabmodel.h"
#include "privatetabmodel.h"
#include "declarativehistorymodel.h"
#include "declarativetabfiltermodel.h"
#include "WPEWebContainer.h"
#include "WPEWebPage.h"
#include "WPEWebPageCreator.h"
#include "declarativeloginmodel.h"
#include "loginfiltermodel.h"
#include "datafetcher.h"
#include "inputregion.h"
#include "searchenginemodel.h"
#include "secureaction.h"
#include "faviconmanager.h"
#include "bookmarkmanager.h"
#include "WPEChromeOverlay.h"
#include "WPEWaylandSubsurface.h"
#include "declarativewebutils.h"
#include "settingmanager.h"
#include "downloadmanager.h"

#include <QGuiApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTouchEvent>
#include <QDBusConnection>
#include <QJSEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QWindow>
#include <qpa/qplatformnativeinterface.h>
#include <stdio.h>

namespace {

const char *applicationStateName(Qt::ApplicationState state)
{
    switch (state) {
    case Qt::ApplicationSuspended:
        return "suspended";
    case Qt::ApplicationInactive:
        return "inactive";
    case Qt::ApplicationActive:
        return "active";
    default:
        return "unknown";
    }
}

void requestActivePageRenderRecovery(QQuickView *view, const char *reason)
{
    if (!view) {
        return;
    }

    static bool s_reloadingQml = false;

    // findChildren() cannot be used here: pages only get a visual parent
    // (setParentItem), so the QObject tree under the view never contains
    // them. Before the registry existed this branch concluded "no pages
    // remain" on EVERY app resume and reloaded the QML source over a live
    // session (start page shown / pre-guard sandbox abort).
    const QList<WPEWebPage *> pages = WPEWebPage::liveInstances();
    int resumedPageCount = 0;
    for (WPEWebPage *page : pages) {
        if (!page || !page->active()) {
            continue;
        }

        page->resumeView();
        ++resumedPageCount;
    }

    if (resumedPageCount > 0) {
        view->update();
    } else if (pages.isEmpty() && !s_reloadingQml) {
        fprintf(stderr, "[ATLANTIC-RUNTIME] Render recovery (%s): no pages remain; reloading QML source\n",
                reason ? reason : "unknown");
        s_reloadingQml = true;
        if (!view->source().isEmpty()) {
            const QUrl currentSource = view->source();
            view->setSource(QUrl());
            view->engine()->clearComponentCache();
            view->setSource(currentSource);
        }
        s_reloadingQml = false;
    }

    fprintf(stderr, "[ATLANTIC-RUNTIME] Render recovery (%s): resumed %d active page(s) out of %d\n",
            reason ? reason : "unknown", resumedPageCount, pages.size());
}

void installRenderRecoveryHooks(QQuickView *view, QGuiApplication *app)
{
    if (!view || !app) {
        return;
    }

    Qt::ApplicationState previousState = app->applicationState();

    QObject::connect(app, &QGuiApplication::applicationStateChanged,
                     view,
                     [view, previousState](Qt::ApplicationState newState) mutable {
        const Qt::ApplicationState oldState = previousState;
        previousState = newState;

        if (newState != Qt::ApplicationActive) {
            return;
        }

        const bool resumedFromBackground = oldState == Qt::ApplicationInactive
                || oldState == Qt::ApplicationSuspended;
        if (!resumedFromBackground) {
            return;
        }

        fprintf(stderr, "[ATLANTIC-RUNTIME] Application state transition: %s -> %s\n",
                applicationStateName(oldState), applicationStateName(newState));
        requestActivePageRenderRecovery(view, "app-activated");
    },
                     Qt::UniqueConnection);

    QObject::connect(view, &QQuickWindow::sceneGraphError,
                     view,
                     [view](QQuickWindow::SceneGraphError error, const QString &message) {
        fprintf(stderr, "[ATLANTIC-RUNTIME] Scene graph error (%d): %s\n",
                static_cast<int>(error), message.toUtf8().constData());
        requestActivePageRenderRecovery(view, "scenegraph-error");
    },
                     Qt::UniqueConnection);

    QObject::connect(view, &QQuickWindow::sceneGraphInvalidated,
                     view,
                     [view, app]() {
        fprintf(stderr, "[ATLANTIC-RUNTIME] Scene graph invalidated\n");
        // Minimizing tears down the scene graph while applicationState still
        // reads Active (the state change lands later), so the app-state check
        // alone is not enough. An unexposed window means this invalidation is
        // part of normal hide/minimize — recovering here would reload the QML
        // source over a live session. The app-activated hook handles resume.
        if (app && app->applicationState() == Qt::ApplicationActive && view->isExposed()) {
            requestActivePageRenderRecovery(view, "scenegraph-invalidated");
        }
    },
                     Qt::UniqueConnection);
}

} // namespace

static QObject *search_model_factory(QQmlEngine *, QJSEngine *)
{
    return new SearchEngineModel;
}

static QObject *faviconmanager_factory(QQmlEngine *, QJSEngine *)
{
    return FaviconManager::instance();
}

static QObject *bookmarkmanager_factory(QQmlEngine *, QJSEngine *)
{
    return BookmarkManager::instance();
}

static void registerBrowserQmlTypes()
{
    static bool registered = false;
    if (registered) {
        return;
    }

    const char *uri = "Sailfish.Browser";

    qmlRegisterRevision<QQuickItem, 1>(uri, 1, 0);
    qmlRegisterRevision<QWindow, 1>(uri, 1, 0);

    qmlRegisterUncreatableType<DeclarativeTabModel>(uri, 1, 0, "TabModel", "TabModel is abstract!");
    qmlRegisterUncreatableType<PrivateTabModel>(uri, 1, 0, "PrivateTabModel", "");
    qmlRegisterType<DeclarativeBookmarkModel>(uri, 1, 0, "BookmarkModel");
    qmlRegisterUncreatableType<PersistentTabModel>(uri, 1, 0, "PersistentTabModel", "");
    qmlRegisterType<DeclarativeHistoryModel>(uri, 1, 0, "HistoryModel");
    qmlRegisterType<BookmarkFilterModel>(uri, 1, 0, "BookmarkFilterModel");
    qmlRegisterType<DeclarativeLoginModel>(uri, 1, 0, "LoginModel");
    qmlRegisterType<LoginFilterModel>(uri, 1, 0, "LoginFilterModel");
    qmlRegisterType<DeclarativeTabFilterModel>(uri, 1, 0, "TabFilterModel");
    qmlRegisterSingletonType<BookmarkManager>(uri, 1, 0, "BookmarkManager", bookmarkmanager_factory);
    qmlRegisterSingletonType<FaviconManager>(uri, 1, 0, "FaviconManager", faviconmanager_factory);
    qmlRegisterUncreatableType<DownloadStatus>(uri, 1, 0, "DownloadStatus", "");
    qmlRegisterType<WPEWebContainer>(uri, 1, 0, "WebContainer");
    qmlRegisterType<WPEWebPage>(uri, 1, 0, "WebPage");
    qmlRegisterType<WPEWebPageCreator>(uri, 1, 0, "WebPageCreator");
    qmlRegisterType<DesktopBookmarkWriter>(uri, 1, 0, "DesktopBookmarkWriter");
    qmlRegisterType<DataFetcher>(uri, 1, 0, "DataFetcher");
    qmlRegisterType<InputRegion>(uri, 1, 0, "InputRegion");
    qmlRegisterType<SecureAction>(uri, 1, 0, "SecureAction");
    qmlRegisterSingletonType<SearchEngineModel>(uri, 1, 0, "SearchEngineModel", search_model_factory);

    registered = true;
}

// Direct-composite: the chrome + web view live in the OFFSCREEN overlay QQuickWindow,
// which gets no OS input. Forward the shell window's input into it. Touch is delivered as
// synthesized mouse (single-point) — that drives Silica MouseArea/Flickable AND the web
// view (WPEQtView::mousePressEvent → WPE). Coords are scaled by the shell's devicePixelRatio
// to map shell-logical px to the overlay scene's (dpr-1) coordinate space.
namespace {
class ShellInputForwarder : public QObject
{
public:
    ShellInputForwarder(QQuickWindow *target, qreal scale, QObject *parent)
        : QObject(parent), m_target(target), m_scale(scale) {}

protected:
    bool eventFilter(QObject *, QEvent *e) override
    {
        switch (e->type()) {
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd: {
            auto *te = static_cast<QTouchEvent *>(e);
            if (te->touchPoints().isEmpty())
                return false;
            const QTouchEvent::TouchPoint &tp = te->touchPoints().first();
            const QEvent::Type mt = e->type() == QEvent::TouchBegin ? QEvent::MouseButtonPress
                                  : e->type() == QEvent::TouchEnd   ? QEvent::MouseButtonRelease
                                                                    : QEvent::MouseMove;
            const QPointF p(tp.pos().x() * m_scale, tp.pos().y() * m_scale);
            QMouseEvent me(mt, p, p, p, Qt::LeftButton,
                           mt == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                           Qt::NoModifier);
            QCoreApplication::sendEvent(m_target, &me);
            return true;
        }
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseMove: {
            auto *om = static_cast<QMouseEvent *>(e);
            const QPointF p(om->localPos().x() * m_scale, om->localPos().y() * m_scale);
            QMouseEvent me(om->type(), p, p, p, om->button(), om->buttons(), om->modifiers());
            QCoreApplication::sendEvent(m_target, &me);
            return true;
        }
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
            QCoreApplication::sendEvent(m_target, e);
            return false;
        default:
            return false;
        }
    }

    QQuickWindow *m_target;
    qreal m_scale;
};
} // namespace

// Direct-composite: host the full browser.qml UI in an offscreen chrome overlay window
// (rendered above the directly-composited web), instead of in the on-screen `view` which
// becomes a transparent shell. The web view inside browser.qml re-parents its subsurface
// to the shell (below the chrome). No-op unless ATLANTIC_DIRECT_COMPOSITE is set.
static bool setupChromeOverlayScene(QQuickView *view, const char *dataPath)
{
    const QByteArray dc = qgetenv("ATLANTIC_DIRECT_COMPOSITE");
    if (dc.isEmpty() || dc == "0")
        return false;

    QPlatformNativeInterface *ni = QGuiApplication::platformNativeInterface();
    if (!ni)
        return false;
    void *display = ni->nativeResourceForIntegration(QByteArrayLiteral("display"));
    if (!display)
        display = ni->nativeResourceForIntegration(QByteArrayLiteral("wl_display"));
    void *shellSurface = ni->nativeResourceForWindow(QByteArrayLiteral("surface"), view);
    if (!display || !shellSurface) {
        fprintf(stderr, "[ATLANTIC-RUNTIME] direct-composite: no wayland display/surface; UI stays in view\n");
        return false;
    }

    const qreal dpr = view->effectiveDevicePixelRatio();
    QSize sz(qRound(view->width() * dpr), qRound(view->height() * dpr));
    if (sz.isEmpty() && view->screen())
        sz = view->screen()->size() * dpr;
    if (sz.isEmpty())
        return false;

    // The WPEView lives in the offscreen overlay window (no wl_surface), so its web
    // subsurface must parent to this on-screen shell.
    WPEWaylandSubsurface::setShellWindow(view);

    auto *overlay = new WPEChromeOverlay();
    if (!overlay->create(reinterpret_cast<wl_display *>(display),
                         reinterpret_cast<wl_surface *>(shellSurface), sz)) {
        fprintf(stderr, "[ATLANTIC-RUNTIME] direct-composite: chrome overlay create failed; UI stays in view\n");
        delete overlay;
        WPEWaylandSubsurface::setShellWindow(nullptr);
        return false;
    }
    WPEChromeOverlay::setPrimary(overlay);

    QQmlEngine *eng = overlay->qmlEngine();
    eng->rootContext()->setContextProperty("WebUtils", DeclarativeWebUtils::instance());
    eng->rootContext()->setContextProperty("Settings", SettingManager::instance());
    eng->rootContext()->setContextProperty("DownloadManager", DownloadManager::instance());

    const QString path = QString::fromLocal8Bit(dataPath ? dataPath : "") + QStringLiteral("browser.qml");
    auto *comp = new QQmlComponent(eng, QUrl::fromLocalFile(path));
    if (comp->isError()) {
        fprintf(stderr, "[ATLANTIC-RUNTIME] direct-composite: browser.qml error: %s\n",
                qPrintable(comp->errorString()));
        return false;
    }
    QObject *root = comp->create();
    QQuickItem *item = qobject_cast<QQuickItem *>(root);
    if (!item) {
        fprintf(stderr, "[ATLANTIC-RUNTIME] direct-composite: browser.qml root is not a QQuickItem\n");
        delete root;
        return false;
    }
    item->setParentItem(overlay->quickWindow()->contentItem());
    overlay->contentReady();

    // Forward the shell window's touch/mouse/key into the offscreen overlay scene so the
    // UI (and the web view inside it) is interactive.
    view->installEventFilter(new ShellInputForwarder(overlay->quickWindow(),
                                                     view->devicePixelRatio(), view));

    fprintf(stderr, "[ATLANTIC-RUNTIME] direct-composite: browser.qml hosted in chrome overlay (%dx%d)\n",
            sz.width(), sz.height());
    return true;
}

extern "C" Q_DECL_EXPORT bool atlanticBrowserRuntimeStart(QQuickView *view,
                                                          QGuiApplication *app,
                                                          const char *dataPath)
{
    if (!view || !app) {
        return false;
    }

    if (view->property("atlanticBrowserRuntimeLoaded").toBool()) {
        return true;
    }

    registerBrowserQmlTypes();

    fprintf(stderr, "[ATLANTIC-RUNTIME] Starting browser runtime\n");
    BrowserService *service = new BrowserService(app);
    if (service->registered()) {
        fprintf(stderr, "[ATLANTIC-RUNTIME] BrowserService registered\n");
    } else {
        fprintf(stderr, "[ATLANTIC-RUNTIME] BrowserService registration failed; transfer callbacks unavailable\n");
    }

    Browser *browser = new Browser(view, QString::fromLocal8Bit(dataPath ? dataPath : ""), app);
    static const QString kBrowserUiServiceName = QStringLiteral("org.atlantic.browser.ui");
    QDBusConnection sessionBus = QDBusConnection::sessionBus();
    const bool uiServiceNameRegistered = sessionBus.registerService(kBrowserUiServiceName);
    if (!uiServiceNameRegistered) {
        fprintf(stderr, "[ATLANTIC-RUNTIME] BrowserUIService name registration failed for %s\n",
                qPrintable(kBrowserUiServiceName));
    }
    BrowserUIService *uiService = new BrowserUIService(app);
    if (uiServiceNameRegistered && uiService->registered()) {
        fprintf(stderr, "[ATLANTIC-RUNTIME] BrowserUIService registered\n");
    } else if (!uiService->registered()) {
        fprintf(stderr, "[ATLANTIC-RUNTIME] BrowserUIService object registration failed\n");
    }
    installRenderRecoveryHooks(view, app);

    if (service->registered()) {
        QObject::connect(service, &BrowserService::openUrlRequested,
                         browser, &Browser::openUrl);
        QObject::connect(service, &BrowserService::activateNewTabViewRequested,
                         browser, &Browser::openNewTabView);
        QObject::connect(service, &BrowserService::dumpMemoryInfoRequested,
                         browser, &Browser::dumpMemoryInfo);
        QObject::connect(service, &BrowserService::cancelTransferRequested,
                         browser, &Browser::cancelDownload);
        QObject::connect(service, &BrowserService::restartTransferRequested,
                         browser, &Browser::restartDownload);
    }

    if (uiServiceNameRegistered && uiService->registered()) {
        QObject::connect(uiService, &BrowserUIService::openUrlRequested,
                         browser, &Browser::openUrl);
        QObject::connect(uiService, &BrowserUIService::openSettingsRequested,
                         browser, &Browser::openSettings);
        QObject::connect(uiService, &BrowserUIService::activateNewTabViewRequested,
                         browser, &Browser::openNewTabView);
        QObject::connect(uiService, &BrowserUIService::showChrome,
                         browser, &Browser::showChrome);
    }

    // Direct-composite: host browser.qml in the chrome overlay (Browser ctor skipped its
    // view->setSource in this mode). Must run before browser->load() triggers the first
    // tab/web view, so the shell window + overlay are ready.
    const QByteArray dc = qgetenv("ATLANTIC_DIRECT_COMPOSITE");
    if (!dc.isEmpty() && dc != "0") {
        // Defer the overlay + scene setup (and the load it must precede) to a later event-
        // loop turn so the shell window and its render thread fully initialise FIRST. The
        // DC startup otherwise races (shell QSG render thread vs the GUI-thread overlay/web
        // setup) and intermittently corrupts the heap. Serialising startup avoids the race.
        const QString dataPathCopy = QString::fromLocal8Bit(dataPath ? dataPath : "");
        QQuickView* v = view;
        QTimer::singleShot(900, view, [v, dataPathCopy, browser]() {
            setupChromeOverlayScene(v, dataPathCopy.toLocal8Bit().constData());
            browser->load();
        });
    } else {
        browser->load();
    }
    view->setProperty("atlanticBrowserRuntimeLoaded", true);
    view->raise();
    view->requestActivate();
    return true;
}
