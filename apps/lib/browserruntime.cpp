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

#include <QGuiApplication>
#include <QColor>
#include <QFile>
#include <QUrl>
#include <QDBusConnection>
#include <QJSEngine>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWindow>
#include <QWindow>
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

    browser->load();
    view->setProperty("atlanticBrowserRuntimeLoaded", true);
    view->raise();
    view->requestActivate();

    // Probe (ATLANTIC_DC_CHILDWIN_TEST): can a Qt child window host the chrome on a
    // layer above the web subsurface? Show a transparent child QQuickView (transient
    // child of the main window) with a labelled translucent box over the bottom half.
    // If it composites above the web, QtWayland gives child windows stackable
    // subsurfaces and we can move the real Silica chrome onto such a layer. Throwaway.
    if (!qEnvironmentVariableIsEmpty("ATLANTIC_DC_CHILDWIN_TEST")) {
        static const char* kProbeQml =
            "import QtQuick 2.2\n"
            "Rectangle { color: '#cc00aaff';\n"
            "  Text { anchors.centerIn: parent; text: 'CHILD WINDOW LAYER';\n"
            "         font.pixelSize: 56; color: 'white' } }\n";
        const QString path = QStringLiteral("/tmp/atl-childwin-probe.qml");
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(kProbeQml);
            f.close();
            QQuickView* child = new QQuickView();
            child->setTransientParent(view);
            child->setFlags(child->flags() | Qt::FramelessWindowHint);
            child->setColor(QColor(0, 0, 0, 0));
            child->setResizeMode(QQuickView::SizeRootObjectToView);
            child->resize(view->width(), view->height() / 2);
            child->setPosition(0, view->height() / 2);
            child->setSource(QUrl::fromLocalFile(path));
            child->show();
            fprintf(stderr, "[ATLANTIC-RUNTIME] childwin-test: child QQuickView shown over bottom half\n");
        }
    }
    return true;
}
