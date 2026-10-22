#pragma once

#include <QtGlobal>

// /usr/lib64 on aarch64, /usr/lib on armv7hl (set by defaults.pri from SFOS_LIBDIR).
#ifndef ATLANTIC_LIBDIR
#define ATLANTIC_LIBDIR "/usr/lib64"
#endif

namespace WPERuntimePaths {

static constexpr const char* kRuntimePrefix = "/opt/wpe-sfos";
static constexpr const char* kSystemLibDir = ATLANTIC_LIBDIR;
static constexpr const char* kCompatLibDir = ATLANTIC_LIBDIR "/wpe-compat";
static constexpr const char* kBrowserRuntimeLibrary = ATLANTIC_LIBDIR "/libsailfishbrowser.so.1";
static constexpr const char* kBrowserStartupLog = "/home/defaultuser/.cache/wpe/browser-startup.log";
static constexpr const char* kAtlanticShareDir = "/usr/share/atlantic-browser";
static constexpr const char* kWebExtensionsDir = ATLANTIC_LIBDIR "/atlantic-browser/web-extensions";
static constexpr const char* kQtShareDir = "/usr/share/qt5";
static constexpr const char* kQtLibDir = ATLANTIC_LIBDIR "/qt5";
static constexpr const char* kRuntimeDir = "/run/user/100000";
static constexpr const char* kGStreamerPluginDir = ATLANTIC_LIBDIR "/gstreamer-1.0";
static constexpr const char* kLibGLDriversDir = ATLANTIC_LIBDIR "/dri";
static constexpr qreal kFallbackScreenWidth = 1080.0;
static constexpr qreal kFallbackScreenHeight = 2520.0;
static constexpr qreal kReferenceViewportWidth = 360.0;
// Silica Theme.pixelRatio of the Xperia 10 II, where kReferenceViewportWidth
// gives device scale 3.0. Other devices scale from it (see
// WPEWebContainer::initialPageDeviceScaleFactor).
static constexpr qreal kReferenceUiPixelRatio = 1.75;
static constexpr qreal kReferenceDeviceScale = 3.0;

} // namespace WPERuntimePaths
