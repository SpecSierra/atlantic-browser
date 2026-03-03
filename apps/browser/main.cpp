/****************************************************************************
**
** Copyright (c) 2013 - 2021 Jolla Ltd.
** Copyright (c) 2020 Open Mobile Platform LLC.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <QGuiApplication>
#include <QQuickView>
#include <qqmldebug.h>
#include <QtQml>
#include <QTimer>
#include <QTranslator>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>

#include "browser.h"
// Registered QML types
#include "declarativebookmarkmodel.h"
#include "bookmarkfiltermodel.h"
#include "desktopbookmarkwriter.h"
#include "downloadstatus.h"
#include "browserservice.h"
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

#ifdef HAS_BOOSTER
#include <MDeclarativeCache>
#endif

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

static char** g_restartArgv = nullptr;
// Restart count stored as a global (set before execv via environ, read on startup)
static int g_restartCount = 0;

// SIGABRT handler: Wayland QPA calls abort() when the compositor pipe breaks at startup.
// Intercept and re-exec instead. Must be async-signal-safe (only write/sleep/close/execv/_exit).
static void sigAbrtHandler(int)
{
    if (g_restartCount >= 5)
        _exit(1);

    sleep(2);
    for (int fd = 3; fd < 256; fd++)
        close(fd);
    execv("/usr/bin/atlantic-browser.bin", g_restartArgv);
    _exit(1);
}

static void qmlDebugMessageHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    Q_UNUSED(ctx);
    const char *prefix = "";
    switch (type) {
    case QtDebugMsg: prefix = "[DBG]"; break;
    case QtInfoMsg: prefix = "[INF]"; break;
    case QtWarningMsg: prefix = "[WRN]"; break;
    case QtCriticalMsg: prefix = "[CRT]"; break;
    case QtFatalMsg: prefix = "[FAT]"; break;
    }
    fprintf(stderr, "%s %s\n", prefix, qPrintable(msg));
}

Q_DECL_EXPORT int main(int argc, char *argv[])
{
    qInstallMessageHandler(qmlDebugMessageHandler);
    // Early startup log — written before any Qt init so we can diagnose booster/sailjail failures
    {
        int logfd = open("/home/defaultuser/wpe-sfos-artifacts/browser-startup.log", O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (logfd >= 0) {
            const char *hdr = "=== atlantic-browser main() started ===\n";
            write(logfd, hdr, __builtin_strlen(hdr));
            // Log argv
            for (int i = 0; i < argc; i++) {
                write(logfd, "  argv[", 7);
                char ibuf[4] = {'0'+i, ']', ':', ' '};
                write(logfd, ibuf, 4);
                write(logfd, argv[i], __builtin_strlen(argv[i]));
                write(logfd, "\n", 1);
            }
            // Log key env vars
            const char *envvars[] = {"WAYLAND_DISPLAY","XDG_RUNTIME_DIR","DBUS_SESSION_BUS_ADDRESS",
                                     "LD_LIBRARY_PATH","BROWSER_RESTART_COUNT",nullptr};
            for (int i = 0; envvars[i]; i++) {
                const char *v = getenv(envvars[i]);
                write(logfd, envvars[i], __builtin_strlen(envvars[i]));
                write(logfd, "=", 1);
                write(logfd, v ? v : "(null)", v ? __builtin_strlen(v) : 6);
                write(logfd, "\n", 1);
            }
            close(logfd);
        }
    }

    g_restartArgv = argv;
    // Read restart count from env (set by previous crashed instance via putenv before execv)
    {
        const char* c = getenv("BROWSER_RESTART_COUNT");
        if (c) { for (const char* p = c; *p >= '0' && *p <= '9'; ++p) g_restartCount = g_restartCount * 10 + (*p - '0'); }
    }
    // Ignore SIGPIPE; restore SIGTERM default so task-switcher close properly kills us
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, SIG_DFL);

    // Remove Gecko-specific env vars; set WPE env
    unsetenv("MOZ_DISABLE_CRASH_GUARD");
    unsetenv("MOZ_WEBGL_PREFER_EGL");
    setenv("WEBKIT_DISABLE_SANDBOX", "1", 1);

    // GStreamer: disable system plugins (system libgstsoup uses libsoup-2.4, incompatible with
    // our libsoup-3), use only our custom plugin dir.
    setenv("GST_PLUGIN_SYSTEM_PATH_1_0", "", 1);
    setenv("GST_PLUGIN_PATH", "/home/defaultuser/wpe-sfos-artifacts/gst-plugins", 1);
    setenv("WEBKIT_GST_ENABLE_HLS_SUPPORT", "1", 1);

    // Mesa DRI driver path (needed so swrast/GBM drivers are found for GBM device init)
    setenv("LIBGL_DRIVERS_PATH", "/usr/lib64/dri", 1);

    QQuickWindow::setDefaultAlphaBuffer(true);

    if (!qgetenv("QML_DEBUGGING_ENABLED").isEmpty()) {
        QQmlDebuggingEnabler qmlDebuggingEnabler;
    }

#ifdef HAS_BOOSTER
    QScopedPointer<QGuiApplication> app(MDeclarativeCache::qApplication(argc, argv));
    QScopedPointer<QQuickView> view(MDeclarativeCache::qQuickView());
#else
    QScopedPointer<QGuiApplication> app(new QGuiApplication(argc, argv));
    QScopedPointer<QQuickView> view(new QQuickView);
#endif
    // Allow Qt to quit when the last window is closed (e.g. user closes from task switcher).
    // Backgrounding (swipe to side) only hides the window surface — it does NOT close it —
    // so this does not affect normal background/cover behaviour.
    app->setQuitOnLastWindowClosed(true);
    app->setAttribute(Qt::AA_SynthesizeTouchForUnhandledMouseEvents, true);

    BrowserService *service = new BrowserService(app.data());
    // Handle command line launch
    if (!service->registered()) {

        QDBusMessage message;
        if (app->arguments().contains("-dumpMemory")) {
            int index = app->arguments().indexOf("-dumpMemory");
            QString fileName;
            if (index + 1 < app->arguments().size()) {
                fileName = app->arguments().at(index + 1);
            }

            message = QDBusMessage::createMethodCall(service->serviceName(), "/",
                                                     service->serviceName(), "dumpMemoryInfo");
            message.setArguments(QVariantList() << fileName);
        } else {
            message = QDBusMessage::createMethodCall(service->serviceName(), "/",
                                                     service->serviceName(), "openUrl");
            QStringList args;
            // Pass url argument if given
            if (app->arguments().count() > 1) {
                args << app->arguments().at(1);
            }
            message.setArguments(QVariantList() << args);
        }

        QDBusConnection::sessionBus().asyncCall(message);
        if (QCoreApplication::hasPendingEvents()) {
            QCoreApplication::processEvents();
        }

        return 0;
    }

    // Claim the .ui D-Bus name NOW (before event loop starts processing messages).
    // registerService is safe here because no pending D-Bus messages exist yet.
    // BrowserUIService constructor will detect the name is already claimed and skip registerService.
    QDBusConnection::sessionBus().registerService(QStringLiteral("org.atlantic.browser.ui"));
    BrowserUIService *uiService = new BrowserUIService(app.data());

    QString translationPath("/usr/share/translations/");
    QTranslator engineeringEnglish;
    engineeringEnglish.load("atlantic-browser_eng_en", translationPath);
    qApp->installTranslator(&engineeringEnglish);

    QTranslator translator;
    translator.load(QLocale(), "atlantic-browser", "-", translationPath);
    qApp->installTranslator(&translator);

    //% "Atlantic"
    view->setTitle(qtTrId("atlantic-browser-ap-name"));

    app->setApplicationName(QStringLiteral("browser"));
    app->setOrganizationName(QStringLiteral("org.sailfishos"));

    const char *uri = "Sailfish.Browser";

    // Use QtQuick 2.1 for Sailfish.Browser imports
    qmlRegisterRevision<QQuickItem, 1>(uri, 1, 0);
    qmlRegisterRevision<QWindow, 1>(uri, 1, 0);

    qmlRegisterUncreatableType<DeclarativeTabModel>(uri, 1, 0, "TabModel", "TabModel is abstract!");
    qmlRegisterUncreatableType<PrivateTabModel>(uri, 1, 0, "PrivateTabModel", "");

    // non-captive portal content
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

    Browser *browser = new Browser(view.data(), DEPLOYMENT_PATH, app.data());
    browser->connect(service, &BrowserService::openUrlRequested,
                     browser, &Browser::openUrl);
    browser->connect(service, &BrowserService::activateNewTabViewRequested,
                     browser, &Browser::openNewTabView);
    browser->connect(service, &BrowserService::dumpMemoryInfoRequested,
                     browser, &Browser::dumpMemoryInfo);

     browser->connect(service, &BrowserService::cancelTransferRequested,
                     browser, &Browser::cancelDownload);
    browser->connect(service, &BrowserService::restartTransferRequested,
                     browser, &Browser::restartDownload);

    if (uiService && uiService->registered()) {
        browser->connect(uiService, &BrowserUIService::openUrlRequested,
                         browser, &Browser::openUrl);
        browser->connect(uiService, &BrowserUIService::openSettingsRequested,
                         browser, &Browser::openSettings);
        browser->connect(uiService, &BrowserUIService::activateNewTabViewRequested,
                         browser, &Browser::openNewTabView);
        browser->connect(uiService, &BrowserUIService::showChrome,
                         browser, &Browser::showChrome);
    }

    browser->load();
    // Install SIGABRT handler AFTER all Qt init (Qt installs its own during QGuiApplication
    // construction which would override an earlier install).
    ++g_restartCount;
    // Encode restart count into env so the re-exec'd process inherits it
    static char restartCountEnv[32];
    restartCountEnv[0] = '\0';
    {
        // Format: "BROWSER_RESTART_COUNT=N" — async-signal-safe write later via putenv
        const char* prefix = "BROWSER_RESTART_COUNT=";
        int i = 0;
        while (prefix[i]) restartCountEnv[i] = prefix[i++];
        int n = g_restartCount, d = 1;
        while (n / d >= 10) d *= 10;
        while (d) { restartCountEnv[i++] = '0' + (n / d) % 10; d /= 10; }
        restartCountEnv[i] = '\0';
    }
    putenv(restartCountEnv);
    {
        struct sigaction sa = {};
        sa.sa_handler = sigAbrtHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGABRT, &sa, nullptr);
    }
    return app->exec();
}
