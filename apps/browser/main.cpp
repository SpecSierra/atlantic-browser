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
#include <QDir>
#include <QOffscreenSurface>
#include <QFileInfo>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QColor>
#include <QFile>
#include <QQuickView>
#include <QQuickWindow>
#include <QScreen>
#include <QSet>
#include <QStringList>
#include <QSurfaceFormat>
#include <qqmldebug.h>
#include <QElapsedTimer>
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
#include <dlfcn.h>
#include <thread>

static char** g_restartArgv = nullptr;
// Restart count stored as a global (set before execv via environ, read on startup)
static int g_restartCount = 0;

static bool envVarEnabled(const QByteArray &value)
{
    const QByteArray normalized = value.trimmed().toLower();
    return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

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
    // Debug-level chatter is developer-only; ATLANTIC_DEBUG=1 re-enables it.
    static const bool debugEnabled = qEnvironmentVariableIntValue("ATLANTIC_DEBUG") == 1;
    if (type == QtDebugMsg && !debugEnabled)
        return;
    const char *prefix = "";
    switch (type) {
    case QtDebugMsg: prefix = "[DBG]"; break;
    case QtInfoMsg: prefix = "[INF]"; break;
    case QtWarningMsg: prefix = "[WRN]"; break;
    case QtCriticalMsg: prefix = "[CRT]"; break;
    case QtFatalMsg: prefix = "[FAT]"; break;
    }
    // Cap message length so data-URI payloads (e.g. favicon base64) can't
    // flood the journal.
    if (!debugEnabled && msg.size() > 1024) {
        fprintf(stderr, "%s %s… [truncated %d chars]\n", prefix,
                qPrintable(msg.left(1024)), int(msg.size() - 1024));
        return;
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

// Startup-phase tracing: single monotonic clock started on first use, printed
// as [ATL-STARTUP] <ms> <event>. Cheap fprintfs; left in permanently so launch
// regressions stay diagnosable from /tmp/atl.log without special builds.
static qint64 startupElapsedMs()
{
    static QElapsedTimer timer;
    if (!timer.isValid()) {
        timer.start();
    }
    return timer.elapsed();
}

static void logStartupPhase(const char *event)
{
    fprintf(stderr, "[ATL-STARTUP] %5lld %s\n",
            static_cast<long long>(startupElapsedMs()), event);
}

// Fixed runtime-load delay. Unset (-1) means "load as soon as the splash has
// painted its first frame"; setting ATLANTIC_BROWSER_RUNTIME_DELAY_MS restores
// a fixed timer (A/B lever, and escape hatch if first-frame triggering misfires).
static int browserRuntimeDelayMs()
{
    bool ok = false;
    const int delay = qEnvironmentVariableIntValue("ATLANTIC_BROWSER_RUNTIME_DELAY_MS", &ok);
    return ok && delay >= 0 ? delay : -1;
}

// dlopen the runtime library off the GUI thread while the splash is coming up,
// so the later QLibrary::load() on the GUI thread hits the already-mapped
// library instead of paying the full relocation cost (~0.6s: the runtime pulls
// in libWPEWebKit). dlopen is thread-safe and refcounted; the handle is never
// closed, matching the previous lifetime (the runtime was never unloaded).
static void preloadBrowserRuntimeLibrary()
{
    if (envVarEnabled(qgetenv("ATLANTIC_NO_RUNTIME_PRELOAD"))) {
        return;
    }

    const QString path = browserRuntimeLibraryPath();
    std::thread([path]() {
        logStartupPhase("preload-dlopen-begin");
        if (!dlopen(QFile::encodeName(path).constData(), RTLD_LAZY)) {
            fprintf(stderr, "[ATLANTIC] Runtime preload failed: %s\n", dlerror());
        }
        logStartupPhase("preload-dlopen-end");
    }).detach();
}

static void writeStartupBytes(int fd, const char *data, size_t size)
{
    if (write(fd, data, size) < 0) {
        return;
    }
}

static void logStartupContext(int argc, char *argv[])
{
    const QFileInfo startupLogInfo(QString::fromLatin1(WPERuntimePaths::kBrowserStartupLog));
    if (!startupLogInfo.absolutePath().isEmpty()) {
        QDir().mkpath(startupLogInfo.absolutePath());
    }

    int logfd = open(WPERuntimePaths::kBrowserStartupLog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
        "QT_OPENGL_NO_BGRA",
        "ATLANTIC_KEEP_QT_OPENGL_NO_BGRA",
        "ATLANTIC_GPU_CONSERVATIVE",
        "ATLANTIC_GPU_CONSERVATIVE_PROBE",
        "ATLANTIC_GPU_PROBE_STATUS",
        "WEBKIT_DEBUG",
        "QSG_RENDER_LOOP",
        "QSG_INFO",
        "ATLANTIC_FRAME_PUMP_INTERVAL_MS",
        "ATLANTIC_PERF_LOG",
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
    if (!envVarEnabled(qgetenv("ATLANTIC_KEEP_QT_OPENGL_NO_BGRA"))) {
        unsetenv("QT_OPENGL_NO_BGRA");
    }

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

    // Kinetic-fling deceleration friction (webkit-kinetic-decel-friction-env.patch).
    // Upstream's friction=4 (desktop trackpad tuning) makes a hard touch flick coast
    // only ~half a screen then stop dead — felt as "no momentum". Total coast =
    // velocity/friction and fling time-constant = 1/friction, so a lower value gives
    // the longer, smoother glide phone flicking expects — but after living with it,
    // upstream's 4.0 was dialled back in on-device (3.0/3.5 coasted too far; the
    // earlier 2.0 and 1.5 were slipperier still). Kept env-overridable rather than
    // reverting the patch so it stays tunable via WEBKIT_KINETIC_DECEL_FRICTION.
    if (qgetenv("WEBKIT_KINETIC_DECEL_FRICTION").isEmpty())
        qputenv("WEBKIT_KINETIC_DECEL_FRICTION", "4.0");
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
    bool isAdreno = false;
    bool isMali = false;
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

    // SFOS ships only the versioned libEGL.so.1 (no dev symlink), so the bare
    // "EGL" name never loaded and every device read as egl=unknown.
    QLibrary eglLibrary(QStringLiteral("EGL"), 1);
    if (!eglLibrary.load()) {
        eglLibrary.setFileName(QStringLiteral("EGL"));
        if (!eglLibrary.load()) {
            return;
        }
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
    result.isAdreno = result.glRenderer.contains(QStringLiteral("Adreno"), Qt::CaseInsensitive);
    result.isMali = result.glRenderer.contains(QStringLiteral("Mali"), Qt::CaseInsensitive);
    result.probeSucceeded = !result.glRenderer.isEmpty() && !result.glVersion.isEmpty();

    QStringList reasons;
    if (!result.probeSucceeded) {
        reasons << QStringLiteral("probe-incomplete");
    }
    // Multi-threaded Skia GPU painting makes a shared GL context current on each
    // worker thread, which requires EGL_KHR_surfaceless_context (eglMakeCurrent
    // with EGL_NO_SURFACE). EGL stacks without it — notably the libhybris Adreno
    // on Sailfish — instead drive each worker through a pbuffer/WPE fallback
    // context, and the driver then corrupts shared textures under concurrency
    // (garbled scrollbar/glyphs, dropped tiles on scroll). Treat the missing
    // extension as a hard conservative trigger so we run a single GPU painting
    // thread here; surfaceless-capable stacks (Mali, desktop) keep multi-thread.
    if (!result.hasEglSurfacelessContext) {
        reasons << QStringLiteral("no-egl-surfaceless-context");
    }
    // The libhybris Adreno stays conservative whatever its EGL advertises: its
    // driver ignores cross-context eglWaitSyncKHR, and CPU raster beat every GPU
    // mode there (docs/investigations/gpu-raster-tile-size.md in atlantic-engine).
    // Until the EGL probe was fixed this was implied by egl=unknown.
    if (result.isAdreno) {
        reasons << QStringLiteral("adreno");
    }

    QStringList advisoryReasons;
    if (result.glesMajor < 0) {
        advisoryReasons << QStringLiteral("gles-version-unknown");
    } else if (result.glesMajor < 3) {
        advisoryReasons << QStringLiteral("gles<3");
    }
    if (!result.hasGlExternalImage) {
        advisoryReasons << QStringLiteral("missing-gl-oes-egl-image-external");
    }

    result.conservativeMode = !reasons.isEmpty();
    if (reasons.isEmpty() && advisoryReasons.isEmpty()) {
        result.reason = QStringLiteral("modern-capable");
    } else if (reasons.isEmpty()) {
        result.reason = QStringLiteral("advisory:%1").arg(advisoryReasons.join(QStringLiteral(",")));
    } else if (advisoryReasons.isEmpty()) {
        result.reason = reasons.join(QStringLiteral(","));
    } else {
        result.reason = QStringLiteral("%1;advisory:%2")
                .arg(reasons.join(QStringLiteral(",")), advisoryReasons.join(QStringLiteral(",")));
    }
    return result;
}

// Best-effort: join the memory-contained cgroup set up by the
// atlantic-browser-memory boot service (see atlantic-engine deploy/). Launching
// the UIProcess inside it means every spawned WebProcess/Network/GPU child
// inherits the RAM+swap cap, so a runaway page (e.g. reddit) gets a WebProcess
// OOM-killed inside the cgroup instead of the kernel OOM-killer taking down the
// whole phone. Silent no-op when the cgroup is absent or unwritable (dev hosts,
// other devices, or a sandbox that hides /sys/fs/cgroup) — the enlarged zram set
// up by the same service still provides headroom regardless.
static void joinBrowserMemoryCgroup()
{
    if (qEnvironmentVariableIsSet("ATLANTIC_NO_MEMORY_CGROUP"))
        return;
    QFile procs(QStringLiteral("/sys/fs/cgroup/memory/atlantic/cgroup.procs"));
    if (!procs.open(QIODevice::WriteOnly | QIODevice::Append))
        return;
    procs.write(QByteArray::number(static_cast<qlonglong>(getpid())));
    procs.write("\n");
    procs.close();
    fprintf(stderr, "[ATLANTIC] joined memory cgroup /sys/fs/cgroup/memory/atlantic (pid %d)\n",
            static_cast<int>(getpid()));
}

// RAM tier. The memory defaults (runtime-common.sh, baked into the sailjail
// profile for icon launches) were sized for the 3.5 GB Xperia 10 II, where a
// reddit feed OOM'd the phone until bfcache and the RAM caches were given up
// (ATLANTIC_CACHE_MODEL=viewer) and purging started at a 700 MB WebProcess
// footprint. On a >= 8 GB device (Jolla Phone 2, 12 GB) that trade buys
// nothing and costs Back: the page process sits permanently above 700 MB, so
// even a non-zero bfcache is pruned on every 3 s poll. Measured on the J2
// (2026-09-27, edition.cnn.com -> example.org -> Back): viewer/700 = full
// reload, DCL 3.4-3.7 s, load 5.8-6.4 s; web/4000 = bfcache restore in ~0.4 s,
// for ~0.5 GB more WebProcess RSS (5.6 GB still available). Raising the
// threshold alone changed neither reddit scroll fps nor footprint.
// ATLANTIC_MEMORY_TIER=low keeps the 3.5 GB defaults on any device.
static void configureMemoryTierFromRam()
{
    qint64 memTotalKb = 0;
    QFile meminfo(QStringLiteral("/proc/meminfo"));
    if (meminfo.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QList<QByteArray> lines = meminfo.readAll().split('\n');
        for (const QByteArray &line : lines) {
            if (line.startsWith("MemTotal:")) {
                memTotalKb = line.mid(9).trimmed().split(' ').value(0).toLongLong();
                break;
            }
        }
    }

    static constexpr qint64 kHighTierMinKb = 7LL * 1024 * 1024; // "8 GB" parts report ~7.3-7.6 GiB
    const bool forcedLow = qgetenv("ATLANTIC_MEMORY_TIER") == QByteArrayLiteral("low");
    const bool high = !forcedLow && memTotalKb >= kHighTierMinKb;
    if (high) {
        qputenv("ATLANTIC_CACHE_MODEL", QByteArrayLiteral("web"));
        qputenv("WEBKIT_MEMORY_BASE_THRESHOLD_MB", QByteArrayLiteral("4000"));
        // Keep a spare WebProcess warm (webkit-process-prewarm-env.patch). The
        // first cross-site tap of a session otherwise launches a process on the
        // critical path: J2 build 735, 5 interleaved reps, touch-up -> new page
        // 327 ms (325-329) off vs 107 ms (100-117) on, for +1 process / ~+100 MB
        // idle RSS.
        if (!qEnvironmentVariableIsSet("WEBKIT_PROCESS_PREWARM"))
            qputenv("WEBKIT_PROCESS_PREWARM", QByteArrayLiteral("1"));
    }
    fprintf(stderr, "[ATLANTIC] memory tier: %s (MemTotal %lld MB%s) cache_model=%s base_threshold=%s MB\n",
            high ? "high" : "low", static_cast<long long>(memTotalKb / 1024), forcedLow ? ", forced low" : "",
            qgetenv("ATLANTIC_CACHE_MODEL").constData(), qgetenv("WEBKIT_MEMORY_BASE_THRESHOLD_MB").constData());
}

// Returns true when the GPU is a Mali (the Jolla Phone 2).
static bool configureGpuModeFromCapabilities()
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
    const bool conservativeEffective = effectiveConservative == "1";

    // Select the Skia painting backend from GPU capability, unless the user
    // pinned WEBKIT_SKIA_ENABLE_CPU_RENDERING or WEBKIT_SKIA_GPU_PAINTING_THREADS
    // explicitly. Capable stacks (EGL surfaceless context present: Mali,
    // desktop) get multi-threaded Skia GPU painting with regular fence
    // synchronization. Conservative stacks (libhybris Adreno, or probe
    // failure) get all-CPU raster by default — see the conservative branch
    // below for why, and for the gpu-explicit / gpu-sync env-gated fallbacks.
    // WEBKIT_SKIA_ENABLE_CPU_RENDERING=1 also remains available as a launch-time
    // escape hatch on capable stacks.
    // NOTE: runtime-common.sh must NOT pre-set either variable, or the
    // explicit-override checks below would always win and pin the value.
    static constexpr int kCapableGpuPaintingThreads = 3;
    const bool presetCpuRendering = qEnvironmentVariableIsSet("WEBKIT_SKIA_ENABLE_CPU_RENDERING");
    const bool presetThreads = qEnvironmentVariableIsSet("WEBKIT_SKIA_GPU_PAINTING_THREADS");
    QByteArray paintingMode;
    QByteArray gpuPaintingThreads = qgetenv("WEBKIT_SKIA_GPU_PAINTING_THREADS");
    if (presetCpuRendering) {
        paintingMode = qgetenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING") == "1"
            ? QByteArrayLiteral("cpu(preset)")
            : QByteArrayLiteral("gpu(preset)");
    } else if (presetThreads) {
        paintingMode = QByteArrayLiteral("gpu(preset-threads)");
    } else if (probe.isMali && !hasPresetConservative) {
        // "gpu-mali" (Jolla Phone 2, Mali-G610): GPU tile painting on the
        // compositor thread (as gpu-explicit), plus two settings measured on
        // the device (build 722, heavy fling bench, see
        // docs/investigations/jolla-gpu-black-tiles.md in atlantic-engine):
        //  - WEBKIT_TILE_GPU_READBACK_SYNC=1: a whole-tile GPU replay is
        //    otherwise intermittently lost (black tile until a later repaint;
        //    root cause open). 0/12 black vs 7/18 without it.
        //  - low-res tiles off (WEBKIT_LOWRES_TILE_SCALE=1.0): under GPU paint
        //    the low-res pass plus the sharpen repaint costs more than painting
        //    full-res (55.1 fps / p95 21.5 ms vs 53.4 / 32.5 with it). The
        //    runtime env pre-sets 0.3 on every launch path, so this overrides
        //    it; ATLANTIC_GPU_KEEP_LOWRES=1 keeps it.
        //  - WEBKIT_COMPOSITOR_GL_FINISH=1: glFinish after each composite. The
        //    frame is handed to the Qt process as an EGLImage with no GPU
        //    fence, and with tile raster sharing the GPU queue the composite
        //    finishes late, so Qt sampled a half-rendered frame (bottom of the
        //    screen lagging while fast-scrolling YouTube). Device-verified
        //    cured; ATLANTIC_ACK_ON_SAMPLE=0 alone did not. fps cost unmeasured.
        // Together: 55.1 fps vs 45.4 for CPU painting + low-res. CPU painting
        // stays one env away: WEBKIT_SKIA_ENABLE_CPU_RENDERING=1 or
        // ATLANTIC_GPU_CONSERVATIVE=1.
        qputenv("WEBKIT_RASTER_ON_COMPOSITOR_THREAD", QByteArrayLiteral("1"));
        gpuPaintingThreads = QByteArrayLiteral("1");
        qputenv("WEBKIT_SKIA_GPU_PAINTING_THREADS", gpuPaintingThreads);
        if (!qEnvironmentVariableIsSet("WEBKIT_TILE_GPU_READBACK_SYNC"))
            qputenv("WEBKIT_TILE_GPU_READBACK_SYNC", QByteArrayLiteral("1"));
        if (qgetenv("ATLANTIC_GPU_KEEP_LOWRES") != QByteArrayLiteral("1"))
            qputenv("WEBKIT_LOWRES_TILE_SCALE", QByteArrayLiteral("1.0"));
        if (!qEnvironmentVariableIsSet("WEBKIT_COMPOSITOR_GL_FINISH"))
            qputenv("WEBKIT_COMPOSITOR_GL_FINISH", QByteArrayLiteral("1"));
        paintingMode = QByteArrayLiteral("gpu-mali(auto)");
    } else if (conservativeEffective) {
        const bool forceGlFinish = qEnvironmentVariableIsSet("ATLANTIC_GPU_FORCE_GLFINISH")
            && qgetenv("ATLANTIC_GPU_FORCE_GLFINISH") != QByteArrayLiteral("0");
        const bool forceGpuPaint = qEnvironmentVariableIsSet("ATLANTIC_GPU_FORCE_GPU_PAINT")
            && qgetenv("ATLANTIC_GPU_FORCE_GPU_PAINT") != QByteArrayLiteral("0");

        // Directional tile prepaint (webkit-directional-tile-coverage-env.patch
        // v2): triple cover budget spent vertically, biased 70% ahead of the
        // sustained scroll direction, stable keep rect (no eviction on flips).
        // Hides tile paint-in at the leading edge of fast flicks through
        // image-heavy feeds; user-verified on device build 316. Orthogonal to
        // the paint backend, so applied to all conservative sub-modes. Honoured
        // only if not preset in the environment.
        if (!qEnvironmentVariableIsSet("WEBKIT_DIRECTIONAL_TILE_COVERAGE"))
            qputenv("WEBKIT_DIRECTIONAL_TILE_COVERAGE", QByteArrayLiteral("1"));
        if (!qEnvironmentVariableIsSet("WEBKIT_COVER_AREA_MULTIPLIER"))
            qputenv("WEBKIT_COVER_AREA_MULTIPLIER", QByteArrayLiteral("3"));

        if (forceGlFinish) {
            // Legacy "gpu-sync" escape hatch (ATLANTIC_GPU_FORCE_GLFINISH=1):
            // GPU painting with driver fences disabled + per-frame compositor
            // glFinish + texture-pool reuse disabled. Correct but raster-bound
            // (559 jiffies/scroll baseline). Kept as a fallback in case the
            // default path regresses on some device.
            qputenv("WPE_GL_FENCE_DISABLED", QByteArrayLiteral("1"));
            gpuPaintingThreads = QByteArrayLiteral("1");
            qputenv("WEBKIT_SKIA_GPU_PAINTING_THREADS", gpuPaintingThreads);
            qputenv("WEBKIT_BITMAP_TEXTURE_POOL_DISABLED", QByteArrayLiteral("1"));
            qputenv("WEBKIT_COMPOSITOR_GL_FINISH", QByteArrayLiteral("1"));
            paintingMode = QByteArrayLiteral("gpu-sync(forced)");
        } else if (forceGpuPaint) {
            // "gpu-explicit" (ATLANTIC_GPU_FORCE_GPU_PAINT=1): rasterize tiles on
            // the compositor thread so paint and composite submit to one GL
            // command stream in program order
            // (webkit-raster-on-compositor-thread-env.patch). Removes the
            // cross-context tile race on the libhybris Adreno without the
            // gpu-sync glFinish stalls. Was the conservative default through
            // build ~316; superseded by CPU raster below (device A/B showed the
            // synchronous cross-context GPU tile submit — flushAndSubmit/
            // GrSyncCpu — is the scroll bottleneck here, so keeping raster on
            // the GPU submit path caps fps). Kept as an env-gated fallback.
            qputenv("WEBKIT_RASTER_ON_COMPOSITOR_THREAD", QByteArrayLiteral("1"));
            gpuPaintingThreads = QByteArrayLiteral("1");
            qputenv("WEBKIT_SKIA_GPU_PAINTING_THREADS", gpuPaintingThreads);
            paintingMode = QByteArrayLiteral("gpu-explicit(forced)");
        } else {
            // DEFAULT on conservative stacks (libhybris Adreno 610): all-CPU
            // raster. Device-benchmarked ~2x fps vs gpu-explicit on text/CSS-
            // rich pages (MDN 4.2->8.2 fps, p95 630->220ms) and better worst-
            // frame on image grids (Wikimedia Commons 484->114ms), with NO tile
            // corruption on text, article, or full-size-photo pages. Root cause:
            // the Adreno's synchronous cross-context tile submit
            // (CoordinatedAcceleratedTileBuffer::completePainting ->
            // GrDirectContext::flushAndSubmit(GrSyncCpu)) serializes on the one
            // compositor thread; adding GPU paint threads did NOT move fps
            // (raster parallelized, submit did not), so moving raster entirely
            // off the GPU submit path is the win. CPU worker count comes from
            // WEBKIT_SKIA_CPU_PAINTING_THREADS (runtime-common.sh, =2). No GPU
            // painting thread pool and no WEBKIT_RASTER_ON_COMPOSITOR_THREAD in
            // this mode. Override back to GPU with ATLANTIC_GPU_FORCE_GPU_PAINT=1
            // (gpu-explicit) or ATLANTIC_GPU_FORCE_GLFINISH=1 (gpu-sync).
            qputenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", QByteArrayLiteral("1"));
            paintingMode = QByteArrayLiteral("cpu(auto)");
        }
    } else {
        gpuPaintingThreads = QByteArray::number(kCapableGpuPaintingThreads);
        qputenv("WEBKIT_SKIA_GPU_PAINTING_THREADS", gpuPaintingThreads);
        paintingMode = QByteArrayLiteral("gpu(auto)");
    }

    fprintf(stderr,
            "[ATLANTIC] GPU caps: egl=%s/%s glVendor=%s renderer=%s gles=%s ext{egl_create_ctx=%d,egl_surfaceless=%d,gl_external_image=%d,gl_bgra8888=%d} conservative_auto=%s conservative_effective=%s source=%s painting=%s gpu_painting_threads=%s(%s) reason=%s\n",
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
            paintingMode.constData(),
            gpuPaintingThreads.isEmpty() ? "unset" : gpuPaintingThreads.constData(),
            presetThreads ? "preset" : "auto",
            qPrintable(probe.reason));
    return probe.isMali;
}

// Tell WebKit the panel's refresh rate. Its vblank source on Sailfish is a
// timer that defaults to 60 Hz (webkit-display-refresh-rate-env.patch in
// atlantic-engine), which capped requestAnimationFrame, animations and
// rendering updates at ~62 Hz on the Jolla Phone 2's 90 Hz panel; the
// independent-scroll tick (WEBKIT_INDEPENDENT_SCROLL_TICK_MS, default 16 ms)
// capped scroll offset updates the same way. Both follow the rate chosen here.
// Order: WEBKIT_DISPLAY_REFRESH_RATE if preset, else what Qt reports for the
// screen when it is clearly above 60, else 90 on the Mali device (the J2 panel
// runs at 90, but the compositor may not report it), else WebKit's 60.
static void configureDisplayRefreshRate(QGuiApplication *app, bool isMali)
{
    static constexpr int kJollaPhone2RefreshRate = 90;
    bool ok = false;
    int rate = qEnvironmentVariableIntValue("WEBKIT_DISPLAY_REFRESH_RATE", &ok);
    const char *source = "preset";
    const QScreen *screen = app->primaryScreen();
    const qreal screenRate = screen ? screen->refreshRate() : 0;
    if (!ok || rate < 30 || rate > 240) {
        rate = 0;
        if (screenRate >= 75 && screenRate <= 240) {
            rate = qRound(screenRate);
            source = "screen";
        } else if (isMali) {
            rate = kJollaPhone2RefreshRate;
            source = "mali-default";
        }
        if (rate)
            qputenv("WEBKIT_DISPLAY_REFRESH_RATE", QByteArray::number(rate));
    }

    QByteArray tick = qgetenv("WEBKIT_INDEPENDENT_SCROLL_TICK_MS");
    if (rate && tick.isEmpty()) {
        tick = QByteArray::number(1000.0 / rate, 'f', 2);
        qputenv("WEBKIT_INDEPENDENT_SCROLL_TICK_MS", tick);
    }

    // WPE WebKit 2.54.1 gives the legacy view a non-zero display ID so that
    // DisplayVBlankMonitor tries a DRM vblank source before the timer. The
    // panel here belongs to the Android composer, so keep the timer configured
    // above as the pacing source rather than whatever a DRM node reports.
    if (qEnvironmentVariableIsEmpty("WEBKIT_FORCE_VBLANK_TIMER"))
        qputenv("WEBKIT_FORCE_VBLANK_TIMER", "1");

    fprintf(stderr, "[ATLANTIC] display refresh: %d Hz (%s, screen reports %.1f) scroll_tick=%s ms\n",
            rate ? rate : 60, rate ? source : "webkit-default", screenRate,
            tick.isEmpty() ? "16(default)" : tick.constData());
}

static void configureBrowserApplication(QGuiApplication *app, QQuickView *view)
{
    if (!app || !view) {
        return;
    }

    app->setQuitOnLastWindowClosed(true);
    app->setAttribute(Qt::AA_SynthesizeTouchForUnhandledMouseEvents, true);
    // Atlantic's own identity, the one its sailjail desktop entry declares.
    // It used to be org.sailfishos/browser, i.e. the stock browser's profile.
    app->setApplicationName(QStringLiteral("atlanticbrowser"));
    app->setOrganizationName(QStringLiteral("org.atlantic"));

    QString translationPath("/usr/share/translations/");
    QTranslator *engineeringEnglish = new QTranslator(app);
    engineeringEnglish->load("atlantic-browser_eng_en", translationPath);
    qApp->installTranslator(engineeringEnglish);

    QTranslator *translator = new QTranslator(app);
    translator->load(QLocale(), "atlantic-browser", "-", translationPath);
    qApp->installTranslator(translator);

    //% "Atlantic"
    view->setTitle(qtTrId("atlantic-browser-ap-name"));

    const QByteArray dc = qgetenv("ATLANTIC_DIRECT_COMPOSITE");
    const bool directComposite = !dc.isEmpty() && dc != "0";
    if (directComposite) {
        // Direct-composite: this `view` is just the transparent SHELL — the web subsurface
        // and the chrome overlay subsurface composite above it (the chrome runs in an
        // offscreen render-control window; browser.qml is NOT loaded here). Load a STATIC
        // empty Item, not the animated splash: the splash's spinner would render the shell
        // continuously, and since it's never replaced in this mode its render thread keeps
        // swapping until lipstick stops releasing buffers and queueBuffer/sync_wait hangs
        // the GUI thread. A static shell renders once and goes idle.
        view->setColor(Qt::transparent);
        static const char* kShellQml = "import QtQuick 2.2\nItem { }\n";
        const QString shellPath = QDir::tempPath() + QStringLiteral("/atlantic-dc-shell.qml");
        QFile shellFile(shellPath);
        if (shellFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            shellFile.write(kShellQml);
            shellFile.close();
        }
        view->setResizeMode(QQuickView::SizeRootObjectToView);
        view->setSource(QUrl::fromLocalFile(shellPath));
    } else {
#ifdef USE_RESOURCES
        view->setSource(QUrl(QStringLiteral("qrc:///browser-silica-main-smoke.qml")));
#else
        view->setSource(QUrl::fromLocalFile(QStringLiteral(DEPLOYMENT_PATH) + QStringLiteral("browser-silica-main-smoke.qml")));
#endif
    }
    view->showFullScreen();
    view->raise();
    view->requestActivate();

    if (directComposite) {
        // The shell is static, but subsurface stacking (web below the chrome overlay) is
        // double-buffered on the shell surface and only latches on its commit. Commit it
        // at a low rate so stacking changes (overlay place_above, new tab subsurfaces)
        // apply, without the continuous-render buffer-pool exhaustion that hangs the GUI
        // thread. ~3fps is plenty and trivially cheap for a transparent static shell.
        QTimer* shellCommit = new QTimer(view);
        QObject::connect(shellCommit, &QTimer::timeout, view, [view]() { view->update(); });
        shellCommit->start(300);
    }
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

static void installApplicationShutdownHooks(QGuiApplication *app, QQuickView *view)
{
    if (!app || !view) {
        return;
    }

    QTimer *forcedExitTimer = new QTimer(app);
    forcedExitTimer->setSingleShot(true);
    forcedExitTimer->setInterval(1500);
    QObject::connect(forcedExitTimer, &QTimer::timeout, app, []() {
        fprintf(stderr, "[ATLANTIC] Forcing process exit after quit timeout\n");
        _exit(0);
    }, Qt::UniqueConnection);

    auto requestFastQuit = [app, forcedExitTimer]() {
        fprintf(stderr, "[ATLANTIC] Requesting application quit\n");
        forcedExitTimer->start();
        app->quit();
    };

    QObject::connect(view, SIGNAL(closing(QQuickCloseEvent*)),
                     forcedExitTimer, SLOT(start()),
                     Qt::UniqueConnection);
    QObject::connect(view, SIGNAL(closing(QQuickCloseEvent*)),
                     app, SLOT(quit()),
                     Qt::UniqueConnection);
    QObject::connect(app, &QGuiApplication::lastWindowClosed, app,
                     [app, requestFastQuit]() {
        fprintf(stderr, "[ATLANTIC] Last window closed, quitting application\n");
        requestFastQuit();
    },
                     Qt::UniqueConnection);
    QObject::connect(view, &QObject::destroyed, app,
                     [app, requestFastQuit]() {
        fprintf(stderr, "[ATLANTIC] View destroyed, quitting application\n");
        requestFastQuit();
    },
                     Qt::UniqueConnection);
}

static int runSilicaMainSmokeUi(int argc, char *argv[])
{
    QQuickWindow::setDefaultAlphaBuffer(true);

    QScopedPointer<QGuiApplication> app(new QGuiApplication(argc, argv));
    QScopedPointer<QQuickView> view(new QQuickView);

    app->setQuitOnLastWindowClosed(true);
    app->setApplicationName(QStringLiteral("atlanticbrowser"));
    app->setOrganizationName(QStringLiteral("org.atlantic"));
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

    logStartupPhase("load-runtime-begin");
    runtimeLibrary->setFileName(browserRuntimeLibraryPath());
    if (!runtimeLibrary->load()) {
        fprintf(stderr, "[ATLANTIC] Failed to load browser runtime %s: %s\n",
                qPrintable(runtimeLibrary->fileName()),
                qPrintable(runtimeLibrary->errorString()));
        return false;
    }

    logStartupPhase("load-runtime-dlopened");
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

    logStartupPhase("main-enter");
    qInstallMessageHandler(qmlDebugMessageHandler);
    logStartupContext(argc, argv);

    // Enter the memory-contained cgroup before spawning any child process, so
    // WebProcess/Network/GPU children inherit the RAM+swap cap.
    joinBrowserMemoryCgroup();

    g_restartArgv = argv;
    g_restartCount = restartCountFromEnvironment();
    // Ignore SIGPIPE; restore SIGTERM default so task-switcher close properly kills us
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, SIG_DFL);

    configureBrowserProcessEnvironment();

    // Start mapping the runtime library in the background while Qt and the
    // splash come up; the first-frame load below then finds it already loaded.
    preloadBrowserRuntimeLibrary();

    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setProfile(QSurfaceFormat::NoProfile);
    format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);

    QQuickWindow::setDefaultAlphaBuffer(true);

    if (!qgetenv("QML_DEBUGGING_ENABLED").isEmpty()) {
        QQmlDebuggingEnabler qmlDebuggingEnabler;
    }

    QScopedPointer<QGuiApplication> app(new QGuiApplication(argc, argv));
    logStartupPhase("qguiapp-created");
    const bool isMali = configureGpuModeFromCapabilities();
    configureMemoryTierFromRam();
    configureDisplayRefreshRate(app.data(), isMali);
    QScopedPointer<QQuickView> view(new QQuickView);

    configureBrowserApplication(app.data(), view.data());
    logStartupPhase("view-configured");
    installApplicationShutdownHooks(app.data(), view.data());

    std::unique_ptr<QLibrary> runtimeLibrary(new QLibrary);
    if (!silicaMainSmokeUi) {
        const int fixedDelayMs = browserRuntimeDelayMs();
        auto loadRuntime = [runtimeLibrary = runtimeLibrary.get(), view = view.data(), app = app.data()]() {
            loadBrowserRuntime(runtimeLibrary, view, app);
        };
        if (fixedDelayMs >= 0) {
            QTimer::singleShot(fixedDelayMs, app.data(), loadRuntime);
        } else {
            // Load as soon as the splash has rendered its first frame: the
            // splash paints once, then the GUI thread blocks on runtime startup
            // instead of idling out a fixed delay. afterRendering (not
            // frameSwapped!) is the trigger: on this Wayland stack
            // eglSwapBuffers blocks on lipstick's frame callbacks until the
            // app-launch animation completes (~2.7s), so frameSwapped fires
            // long after the first frame is actually visible. afterRendering
            // fires on the render thread right after the frame is drawn, so
            // queue over to the GUI thread and disconnect on first delivery.
            // The timer is a safety net in case nothing is ever rendered; at
            // the old fixed default the worst case matches the old behavior
            // (loadBrowserRuntime is idempotent).
            auto frameConnection = std::make_shared<QMetaObject::Connection>();
            *frameConnection = QObject::connect(view.data(), &QQuickWindow::afterRendering, app.data(),
                                                [frameConnection, loadRuntime]() {
                logStartupPhase("trigger-afterRendering-delivered");
                QObject::disconnect(*frameConnection);
                loadRuntime();
            }, Qt::QueuedConnection);
            QTimer::singleShot(1500, app.data(), [loadRuntime]() {
                logStartupPhase("trigger-safety-timer");
                loadRuntime();
            });

            // Log-only probes (removed after first emission): when do the
            // render-thread signals actually FIRE, independent of when the
            // GUI thread can service the queued delivery above.
            auto renderProbe = std::make_shared<QMetaObject::Connection>();
            *renderProbe = QObject::connect(view.data(), &QQuickWindow::afterRendering,
                                            view.data(), [renderProbe]() {
                logStartupPhase("emit-afterRendering(render-thread)");
                QObject::disconnect(*renderProbe);
            }, Qt::DirectConnection);
            auto swapProbe = std::make_shared<QMetaObject::Connection>();
            *swapProbe = QObject::connect(view.data(), &QQuickWindow::frameSwapped,
                                          view.data(), [swapProbe]() {
                logStartupPhase("emit-frameSwapped(render-thread)");
                QObject::disconnect(*swapProbe);
            }, Qt::DirectConnection);
        }
    }
    // Install SIGABRT handler AFTER all Qt init (Qt installs its own during QGuiApplication
    // construction which would override an earlier install).
    installBrowserRestartCount();
    installSigAbrtRestartHandler();

    logStartupPhase("exec-enter");
    int result = app->exec();
    
    // Force thread cleanup before exit to prevent hanging processes
    _exit(result);
}
