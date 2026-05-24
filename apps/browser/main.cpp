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
#include <QLibrary>
#include <QOffscreenSurface>
#include <QFileInfo>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QQuickView>
#include <QSet>
#include <QStringList>
#include <qqmldebug.h>
#include <QTimer>
#include <QTranslator>

#include "../wpe/WPERuntimePaths.h"
#include <EGL/egl.h>
#include <memory>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>

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

using AtlanticBrowserRuntimeStartFn = bool (*)(QQuickView *, QGuiApplication *, const char *);

static QString browserRuntimeLibraryPath()
{
    const QByteArray overridePath = qgetenv("ATLANTIC_BROWSER_RUNTIME_LIBRARY");
    return overridePath.isEmpty()
            ? QString::fromLatin1(WPERuntimePaths::kBrowserRuntimeLibrary)
            : QString::fromLocal8Bit(overridePath);
}

static int browserRuntimeDelayMs()
{
    bool ok = false;
    const int delay = qEnvironmentVariableIntValue("ATLANTIC_BROWSER_RUNTIME_DELAY_MS", &ok);
    return ok && delay >= 0 ? delay : 1500;
}

static void writeStartupBytes(int fd, const char *data, size_t size)
{
    if (write(fd, data, size) < 0) {
        return;
    }
}

static void logStartupContext(int argc, char *argv[])
{
    int logfd = open(WPERuntimePaths::kBrowserStartupLog, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (logfd < 0) {
        return;
    }

    const char *hdr = "=== atlantic-browser main() started ===\n";
    writeStartupBytes(logfd, hdr, __builtin_strlen(hdr));

    for (int i = 0; i < argc; ++i) {
        char ibuf[32];
        const int prefixLength = snprintf(ibuf, sizeof(ibuf), "  argv[%d]: ", i);
        if (prefixLength > 0) {
            writeStartupBytes(logfd, ibuf, prefixLength);
        }
        writeStartupBytes(logfd, argv[i], __builtin_strlen(argv[i]));
        writeStartupBytes(logfd, "\n", 1);
    }

    const char *envvars[] = {
        "WAYLAND_DISPLAY",
        "XDG_RUNTIME_DIR",
        "DBUS_SESSION_BUS_ADDRESS",
        "LD_LIBRARY_PATH",
        "BROWSER_RESTART_COUNT",
        nullptr
    };
    for (int i = 0; envvars[i]; ++i) {
        const char *value = getenv(envvars[i]);
        writeStartupBytes(logfd, envvars[i], __builtin_strlen(envvars[i]));
        writeStartupBytes(logfd, "=", 1);
        writeStartupBytes(logfd, value ? value : "(null)", value ? __builtin_strlen(value) : 6);
        writeStartupBytes(logfd, "\n", 1);
    }

    close(logfd);
}

static int restartCountFromEnvironment()
{
    int restartCount = 0;
    const char *value = getenv("BROWSER_RESTART_COUNT");
    if (!value) {
        return 0;
    }

    for (const char *p = value; *p >= '0' && *p <= '9'; ++p) {
        restartCount = restartCount * 10 + (*p - '0');
    }
    return restartCount;
}

static void configureBrowserProcessEnvironment()
{
    auto needsUtf8Locale = [](const char *value) {
        QByteArray normalized(value ? value : "");
        normalized = normalized.trimmed().toLower();
        return normalized.isEmpty() || normalized == "c" || normalized == "posix";
    };

    auto ensureUtf8Locale = [&](const char *name) {
        if (needsUtf8Locale(getenv(name)))
            setenv(name, "en_US.UTF-8", 1);
    };

    ensureUtf8Locale("LC_ALL");
    ensureUtf8Locale("LC_CTYPE");
    ensureUtf8Locale("LANG");

    auto needsEmptyPluginPath = [](QByteArray value) {
        value = value.trimmed();
        return value.isEmpty();
    };
    auto firstExistingDirectory = [](std::initializer_list<QString> candidates) {
        for (const QString &candidate : candidates) {
            if (candidate.isEmpty()) {
                continue;
            }
            const QFileInfo info(candidate);
            if (info.exists() && info.isDir()) {
                return candidate.toUtf8();
            }
        }
        return QByteArray();
    };

    unsetenv("MOZ_DISABLE_CRASH_GUARD");
    unsetenv("MOZ_WEBGL_PREFER_EGL");
    setenv("WEBKIT_DISABLE_SANDBOX", "1", 1);

    if (needsEmptyPluginPath(qgetenv("GST_PLUGIN_SYSTEM_PATH_1_0")))
        qputenv("GST_PLUGIN_SYSTEM_PATH_1_0", QByteArray());
    if (qgetenv("GST_PLUGIN_PATH").isEmpty()) {
        const QByteArray pluginPath = firstExistingDirectory({
            QString::fromUtf8(WPERuntimePaths::kGStreamerPluginDir),
            QStringLiteral("/usr/lib/gstreamer-1.0")
        });
        if (!pluginPath.isEmpty())
            qputenv("GST_PLUGIN_PATH", pluginPath);
    }
    if (qgetenv("WEBKIT_GST_ENABLE_HLS_SUPPORT").isEmpty())
        qputenv("WEBKIT_GST_ENABLE_HLS_SUPPORT", "1");
    if (qgetenv("LIBGL_DRIVERS_PATH").isEmpty()) {
        const QByteArray driverPath = firstExistingDirectory({
            QString::fromUtf8(WPERuntimePaths::kLibGLDriversDir),
            QStringLiteral("/usr/lib/dri")
        });
        if (!driverPath.isEmpty())
            qputenv("LIBGL_DRIVERS_PATH", driverPath);
    }
}

struct GpuCapabilityProbeResult {
    bool probeSucceeded = false;
    bool conservativeMode = true;
    bool eglInfoAvailable = false;
    bool hasEglCreateContext = false;
    bool hasEglSurfacelessContext = false;
    bool hasGlExternalImage = false;
    bool hasGlBgra8888 = false;
    int glesMajor = -1;
    int glesMinor = -1;
    QString reason = QStringLiteral("probe-not-run");
    QString eglVendor;
    QString eglVersion;
    QString glVendor;
    QString glRenderer;
    QString glVersion;
};

static bool extensionListContains(const QByteArray &extensions, const char *extension)
{
    if (!extension || !*extension || extensions.isEmpty()) {
        return false;
    }
    return extensions.split(' ').contains(QByteArray(extension));
}

static bool parseOpenGlesVersion(const QByteArray &versionString, int *majorVersion, int *minorVersion)
{
    if (!majorVersion || !minorVersion) {
        return false;
    }

    *majorVersion = -1;
    *minorVersion = -1;

    for (int i = 0; i < versionString.size(); ++i) {
        if (versionString.at(i) < '0' || versionString.at(i) > '9') {
            continue;
        }

        int major = 0;
        while (i < versionString.size() && versionString.at(i) >= '0' && versionString.at(i) <= '9') {
            major = major * 10 + (versionString.at(i) - '0');
            ++i;
        }

        int minor = 0;
        bool minorFound = false;
        if (i < versionString.size() && versionString.at(i) == '.') {
            ++i;
            while (i < versionString.size() && versionString.at(i) >= '0' && versionString.at(i) <= '9') {
                minor = minor * 10 + (versionString.at(i) - '0');
                minorFound = true;
                ++i;
            }
        }

        *majorVersion = major;
        *minorVersion = minorFound ? minor : 0;
        return true;
    }

    return false;
}

static QString diagnosticValue(const QString &value)
{
    return value.isEmpty() ? QStringLiteral("unknown") : value;
}

static void queryEglDetailsFromCurrentContext(GpuCapabilityProbeResult *result)
{
    if (!result) {
        return;
    }

    QLibrary eglLibrary(QStringLiteral("EGL"));
    if (!eglLibrary.load()) {
        return;
    }

    using EglGetCurrentDisplayFn = EGLDisplay (*)(void);
    using EglQueryStringFn = const char *(*)(EGLDisplay, EGLint);

    const auto eglGetCurrentDisplayFn = reinterpret_cast<EglGetCurrentDisplayFn>(eglLibrary.resolve("eglGetCurrentDisplay"));
    const auto eglQueryStringFn = reinterpret_cast<EglQueryStringFn>(eglLibrary.resolve("eglQueryString"));
    if (!eglGetCurrentDisplayFn || !eglQueryStringFn) {
        return;
    }

    const EGLDisplay display = eglGetCurrentDisplayFn();
    if (display == EGL_NO_DISPLAY) {
        return;
    }

    const char *eglVendor = eglQueryStringFn(display, EGL_VENDOR);
    const char *eglVersion = eglQueryStringFn(display, EGL_VERSION);
    const char *eglExtensions = eglQueryStringFn(display, EGL_EXTENSIONS);

    if (eglVendor) {
        result->eglVendor = QString::fromLocal8Bit(eglVendor);
    }
    if (eglVersion) {
        result->eglVersion = QString::fromLocal8Bit(eglVersion);
    }

    const QByteArray extensions = eglExtensions ? QByteArray(eglExtensions) : QByteArray();
    result->eglInfoAvailable = eglVendor || eglVersion || eglExtensions;
    result->hasEglCreateContext = extensionListContains(extensions, "EGL_KHR_create_context");
    result->hasEglSurfacelessContext = extensionListContains(extensions, "EGL_KHR_surfaceless_context");
}

static GpuCapabilityProbeResult probeGpuCapability()
{
    GpuCapabilityProbeResult result;

    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setProfile(QSurfaceFormat::NoProfile);

    QOffscreenSurface surface;
    surface.setFormat(format);
    surface.create();
    if (!surface.isValid()) {
        result.reason = QStringLiteral("offscreen-surface-create-failed");
        return result;
    }

    QOpenGLContext context;
    context.setFormat(surface.format());
    if (!context.create()) {
        result.reason = QStringLiteral("context-create-failed");
        return result;
    }

    if (!context.makeCurrent(&surface)) {
        result.reason = QStringLiteral("context-make-current-failed");
        return result;
    }

    QOpenGLFunctions *glFunctions = context.functions();
    if (!glFunctions) {
        context.doneCurrent();
        result.reason = QStringLiteral("gl-functions-unavailable");
        return result;
    }

    const char *glVendor = reinterpret_cast<const char *>(glFunctions->glGetString(GL_VENDOR));
    const char *glRenderer = reinterpret_cast<const char *>(glFunctions->glGetString(GL_RENDERER));
    const char *glVersion = reinterpret_cast<const char *>(glFunctions->glGetString(GL_VERSION));

    if (glVendor) {
        result.glVendor = QString::fromLocal8Bit(glVendor);
    }
    if (glRenderer) {
        result.glRenderer = QString::fromLocal8Bit(glRenderer);
    }
    if (glVersion) {
        result.glVersion = QString::fromLocal8Bit(glVersion);
    }

    const QSet<QByteArray> glExtensions = context.extensions();
    result.hasGlExternalImage = glExtensions.contains(QByteArrayLiteral("GL_OES_EGL_image_external"))
            || glExtensions.contains(QByteArrayLiteral("GL_OES_EGL_image"));
    result.hasGlBgra8888 = glExtensions.contains(QByteArrayLiteral("GL_EXT_texture_format_BGRA8888"))
            || glExtensions.contains(QByteArrayLiteral("GL_APPLE_texture_format_BGRA8888"));

    queryEglDetailsFromCurrentContext(&result);
    context.doneCurrent();

    parseOpenGlesVersion(result.glVersion.toLatin1(), &result.glesMajor, &result.glesMinor);
    result.probeSucceeded = !result.glRenderer.isEmpty() && !result.glVersion.isEmpty();

    QStringList reasons;
    if (!result.probeSucceeded) {
        reasons << QStringLiteral("probe-incomplete");
    }
    if (result.glesMajor < 0) {
        reasons << QStringLiteral("gles-version-unknown");
    } else if (result.glesMajor < 3) {
        reasons << QStringLiteral("gles<3");
    }
    if (!result.hasGlExternalImage) {
        reasons << QStringLiteral("missing-gl-oes-egl-image-external");
    }

    result.conservativeMode = !reasons.isEmpty();
    result.reason = reasons.isEmpty() ? QStringLiteral("modern-capable") : reasons.join(QStringLiteral(","));
    return result;
}

static void configureGpuModeFromCapabilities()
{
    const GpuCapabilityProbeResult probe = probeGpuCapability();
    const QByteArray conservativeAuto = probe.conservativeMode ? QByteArrayLiteral("1") : QByteArrayLiteral("0");
    const bool hasPresetConservative = qEnvironmentVariableIsSet("ATLANTIC_GPU_CONSERVATIVE");

    qputenv("ATLANTIC_GPU_CONSERVATIVE_PROBE", conservativeAuto);
    qputenv("ATLANTIC_GPU_PROBE_STATUS", probe.probeSucceeded ? QByteArrayLiteral("ok") : QByteArrayLiteral("failed"));

    if (!hasPresetConservative) {
        qputenv("ATLANTIC_GPU_CONSERVATIVE", conservativeAuto);
    }

    const QByteArray effectiveConservative = qgetenv("ATLANTIC_GPU_CONSERVATIVE");
    fprintf(stderr,
            "[ATLANTIC] GPU caps: egl=%s/%s glVendor=%s renderer=%s gles=%s ext{egl_create_ctx=%d,egl_surfaceless=%d,gl_external_image=%d,gl_bgra8888=%d} conservative_auto=%s conservative_effective=%s source=%s reason=%s\n",
            qPrintable(diagnosticValue(probe.eglVendor)),
            qPrintable(diagnosticValue(probe.eglVersion)),
            qPrintable(diagnosticValue(probe.glVendor)),
            qPrintable(diagnosticValue(probe.glRenderer)),
            qPrintable(diagnosticValue(probe.glVersion)),
            probe.hasEglCreateContext ? 1 : 0,
            probe.hasEglSurfacelessContext ? 1 : 0,
            probe.hasGlExternalImage ? 1 : 0,
            probe.hasGlBgra8888 ? 1 : 0,
            conservativeAuto.constData(),
            effectiveConservative.isEmpty() ? "unknown" : effectiveConservative.constData(),
            hasPresetConservative ? "preset" : "auto",
            qPrintable(probe.reason));
}

static void configureBrowserApplication(QGuiApplication *app, QQuickView *view)
{
    if (!app || !view) {
        return;
    }

    app->setQuitOnLastWindowClosed(true);
    app->setAttribute(Qt::AA_SynthesizeTouchForUnhandledMouseEvents, true);
    app->setApplicationName(QStringLiteral("browser"));
    app->setOrganizationName(QStringLiteral("org.sailfishos"));

    QString translationPath("/usr/share/translations/");
    QTranslator *engineeringEnglish = new QTranslator(app);
    engineeringEnglish->load("atlantic-browser_eng_en", translationPath);
    qApp->installTranslator(engineeringEnglish);

    QTranslator *translator = new QTranslator(app);
    translator->load(QLocale(), "atlantic-browser", "-", translationPath);
    qApp->installTranslator(translator);

    //% "Atlantic"
    view->setTitle(qtTrId("atlantic-browser-ap-name"));
#ifdef USE_RESOURCES
    view->setSource(QUrl(QStringLiteral("qrc:///browser-silica-main-smoke.qml")));
#else
    view->setSource(QUrl::fromLocalFile(QStringLiteral(DEPLOYMENT_PATH) + QStringLiteral("browser-silica-main-smoke.qml")));
#endif
    view->showFullScreen();
    view->raise();
    view->requestActivate();
}

static void installBrowserRestartCount()
{
    ++g_restartCount;

    static char restartCountEnv[32];
    restartCountEnv[0] = '\0';

    const char *prefix = "BROWSER_RESTART_COUNT=";
    int i = 0;
    while (prefix[i]) {
        restartCountEnv[i] = prefix[i];
        ++i;
    }

    int n = g_restartCount;
    int d = 1;
    while (n / d >= 10) {
        d *= 10;
    }
    while (d) {
        restartCountEnv[i++] = '0' + (n / d) % 10;
        d /= 10;
    }
    restartCountEnv[i] = '\0';
    putenv(restartCountEnv);
}

static void installSigAbrtRestartHandler()
{
    struct sigaction sa = {};
    sa.sa_handler = sigAbrtHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGABRT, &sa, nullptr);
}

static int runSilicaMainSmokeUi(int argc, char *argv[])
{
    QQuickWindow::setDefaultAlphaBuffer(true);

    QScopedPointer<QGuiApplication> app(new QGuiApplication(argc, argv));
    QScopedPointer<QQuickView> view(new QQuickView);

    app->setQuitOnLastWindowClosed(true);
    app->setApplicationName(QStringLiteral("browser"));
    app->setOrganizationName(QStringLiteral("org.sailfishos"));
    view->setTitle(QStringLiteral("Atlantic"));
#ifdef USE_RESOURCES
    view->setSource(QUrl(QStringLiteral("qrc:///browser-silica-main-smoke.qml")));
#else
    view->setSource(QUrl::fromLocalFile(QStringLiteral(DEPLOYMENT_PATH) + QStringLiteral("browser-silica-main-smoke.qml")));
#endif
    view->showFullScreen();
    view->raise();
    view->requestActivate();
    return app->exec();
}

static bool loadBrowserRuntime(QLibrary *runtimeLibrary, QQuickView *view, QGuiApplication *app)
{
    if (!runtimeLibrary || !view || !app) {
        return false;
    }

    if (view->property("atlanticBrowserRuntimeLoaded").toBool()) {
        return true;
    }

    runtimeLibrary->setFileName(browserRuntimeLibraryPath());
    if (!runtimeLibrary->load()) {
        fprintf(stderr, "[ATLANTIC] Failed to load browser runtime %s: %s\n",
                qPrintable(runtimeLibrary->fileName()),
                qPrintable(runtimeLibrary->errorString()));
        return false;
    }

    auto startRuntime = reinterpret_cast<AtlanticBrowserRuntimeStartFn>(
                runtimeLibrary->resolve("atlanticBrowserRuntimeStart"));
    if (!startRuntime) {
        fprintf(stderr, "[ATLANTIC] Failed to resolve atlanticBrowserRuntimeStart from %s: %s\n",
                qPrintable(runtimeLibrary->fileName()),
                qPrintable(runtimeLibrary->errorString()));
        return false;
    }

    if (!startRuntime(view, app, DEPLOYMENT_PATH)) {
        fprintf(stderr, "[ATLANTIC] Browser runtime start failed for %s\n",
                qPrintable(runtimeLibrary->fileName()));
        return false;
    }

    fprintf(stderr, "[ATLANTIC] Browser runtime started from %s\n",
            qPrintable(runtimeLibrary->fileName()));
    return true;
}

Q_DECL_EXPORT int main(int argc, char *argv[])
{
    const bool silicaMainSmokeUi = !qEnvironmentVariableIsEmpty("ATLANTIC_SILICA_MAIN_SMOKE");
    if (silicaMainSmokeUi) {
        return runSilicaMainSmokeUi(argc, argv);
    }

    qInstallMessageHandler(qmlDebugMessageHandler);
    logStartupContext(argc, argv);

    g_restartArgv = argv;
    g_restartCount = restartCountFromEnvironment();
    // Ignore SIGPIPE; restore SIGTERM default so task-switcher close properly kills us
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, SIG_DFL);

    configureBrowserProcessEnvironment();

    QQuickWindow::setDefaultAlphaBuffer(true);

    if (!qgetenv("QML_DEBUGGING_ENABLED").isEmpty()) {
        QQmlDebuggingEnabler qmlDebuggingEnabler;
    }

    QScopedPointer<QGuiApplication> app(new QGuiApplication(argc, argv));
    configureGpuModeFromCapabilities();
    QScopedPointer<QQuickView> view(new QQuickView);
    configureBrowserApplication(app.data(), view.data());

    // Ensure clean exit when window closes
    QObject::connect(view.data(), &QQuickView::closing, [app = app.data()]() {
        app->quit();
    });

    std::unique_ptr<QLibrary> runtimeLibrary(new QLibrary);
    if (!silicaMainSmokeUi) {
        QTimer::singleShot(browserRuntimeDelayMs(), app.data(),
                           [runtimeLibrary = runtimeLibrary.get(), view = view.data(), app = app.data()]() {
            loadBrowserRuntime(runtimeLibrary, view, app);
        });
    }
    // Install SIGABRT handler AFTER all Qt init (Qt installs its own during QGuiApplication
    // construction which would override an earlier install).
    installBrowserRestartCount();
    installSigAbrtRestartHandler();
    
    int result = app->exec();
    
    // Force thread cleanup before exit to prevent hanging processes
    _exit(result);
}
