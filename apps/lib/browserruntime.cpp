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

    const QList<WPEWebPage *> pages = view->findChildren<WPEWebPage *>();
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
        if (app && app->applicationState() == Qt::ApplicationActive) {
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

    browser->load();
    view->setProperty("atlanticBrowserRuntimeLoaded", true);
    view->raise();
    view->requestActivate();
    return true;
}
