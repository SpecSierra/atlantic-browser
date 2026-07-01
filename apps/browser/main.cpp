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
#include <QSet>
#include <QStringList>
#include <QSurfaceFormat>
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
    const QFileInfo startupLogInfo(QString::fromLatin1(WPERuntimePaths::kBrowserStartupLog));
    if (!startupLogInfo.absolutePath().isEmpty()) {
        QDir().mkpath(startupLogInfo.absolutePath());
    }

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
    setenv("WEBKIT_DISABLE_SANDBOX", "1", 1);
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
    // the longer, smoother glide phone flicking expects. 3.0 was dialled in on-device
    // (a bit grippier / less coast than the earlier 2.0; 1.5 felt slightly too
    // slippery). Tunable on-device via WEBKIT_KINETIC_DECEL_FRICTION.
    if (qgetenv("WEBKIT_KINETIC_DECEL_FRICTION").isEmpty())
        qputenv("WEBKIT_KINETIC_DECEL_FRICTION", "3.0");
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

    // Enter the memory-contained cgroup before spawning any child process, so
    // WebProcess/Network/GPU children inherit the RAM+swap cap.
    joinBrowserMemoryCgroup();

    g_restartArgv = argv;
    g_restartCount = restartCountFromEnvironment();
    // Ignore SIGPIPE; restore SIGTERM default so task-switcher close properly kills us
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, SIG_DFL);

    configureBrowserProcessEnvironment();

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
    configureGpuModeFromCapabilities();
    QScopedPointer<QQuickView> view(new QQuickView);

    configureBrowserApplication(app.data(), view.data());
    installApplicationShutdownHooks(app.data(), view.data());

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
