#pragma once

#include <QtGlobal>

namespace WPERuntimePaths {

static constexpr const char* kRuntimePrefix = "/opt/wpe-sfos";
static constexpr const char* kSystemLibDir = "/usr/lib64";
static constexpr const char* kCompatLibDir = "/usr/lib64/wpe-compat";
static constexpr const char* kBrowserRuntimeLibrary = "/usr/lib64/libsailfishbrowser.so.1";
static constexpr const char* kBrowserStartupLog = "/home/defaultuser/wpe-sfos-artifacts/browser-startup.log";
static constexpr const char* kAtlanticShareDir = "/usr/share/atlantic-browser";
static constexpr const char* kQtShareDir = "/usr/share/qt5";
static constexpr const char* kQtLibDir = "/usr/lib64/qt5";
static constexpr const char* kRuntimeDir = "/run/user/100000";
static constexpr const char* kGStreamerPluginDir = "/usr/lib64/gstreamer-1.0";
static constexpr const char* kLibGLDriversDir = "/usr/lib64/dri";
static constexpr qreal kFallbackScreenWidth = 1080.0;
static constexpr qreal kFallbackScreenHeight = 2520.0;
static constexpr qreal kReferenceViewportWidth = 360.0;

} // namespace WPERuntimePaths
