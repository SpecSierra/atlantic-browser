/*
 * Copyright (c) 2024 Jolla Ltd.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "WPEWebPage.h"
#include "WPERuntimePaths.h"
#include "downloadmanager.h"
#include "AdBlockEngine.h"

#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineF>
#include <QPointer>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <MDConfItem>
#include <array>
#include <memory>

#include "WPEQtViewLoadRequest.h"

#include <wpe/webkit.h>
#include <gio/gio.h>

namespace {

constexpr double kMinimumPinchZoomFactor = 0.5;
constexpr double kMaximumPinchZoomFactor = 3.0;
constexpr int kDefaultFramePumpIntervalMs = 2000;
constexpr int kMediaInactiveDebounceMs = 400;
const char kContentBlockerIdentifierBase[] = "atlantic-default";
const char kPulseLookupService[] = "org.pulseaudio.Server";
const char kPulseLookupPath[] = "/org/pulseaudio/server_lookup1";
const char kPulseLookupInterface[] = "org.PulseAudio.ServerLookup1";
const char kPropertiesInterface[] = "org.freedesktop.DBus.Properties";
const char kMainVolumePath[] = "/com/meego/mainvolume2";
const char kMainVolumeInterface[] = "com.Meego.MainVolume2";

struct MainVolumeState {
    int currentStep = -1;
    int maximumStep = -1;

    bool valid() const
    {
        return maximumStep >= 0;
    }
};

struct ContentBlockerContext {
    WebKitUserContentManager* manager = nullptr;
    WebKitUserContentFilterStore* store = nullptr;
    GFile* sourceFile = nullptr;
    QByteArray identifier; // versioned: base + mtime suffix

    ~ContentBlockerContext()
    {
        if (manager)
            g_object_unref(manager);
        if (store)
            g_object_unref(store);
        if (sourceFile)
            g_object_unref(sourceFile);
    }
};

QString contentBlockerStorePath()
{
    QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (cacheRoot.isEmpty())
        cacheRoot = QDir::homePath() + QStringLiteral("/.cache");
    return cacheRoot + QStringLiteral("/atlantic-browser/content-filter-store");
}

// All user content managers that participate in ad blocking. The compiled
// content-blocker filter is per-manager (one manager per web view/tab), so
// toggling the setting must walk every live manager — flipping only the
// AdBlockEngine flag left the filters installed and the blocker effectively
// always on.
QSet<WebKitUserContentManager*>& contentBlockerManagers()
{
    static QSet<WebKitUserContentManager*> managers;
    return managers;
}

void onContentBlockerManagerDestroyed(gpointer, GObject* manager)
{
    contentBlockerManagers().remove(reinterpret_cast<WebKitUserContentManager*>(manager));
}

void registerContentBlockerManager(WebKitUserContentManager* manager)
{
    if (!manager || contentBlockerManagers().contains(manager))
        return;
    contentBlockerManagers().insert(manager);
    g_object_weak_ref(G_OBJECT(manager), onContentBlockerManagerDestroyed, nullptr);
}

void installContentBlockerFilter(ContentBlockerContext* context, WebKitUserContentFilter* filter, const char* source)
{
    if (!context || !context->manager || !filter)
        return;

    // The load/compile is async: the user may have toggled ad block off while
    // it was in flight. Don't install a filter that was just turned off.
    if (!AdBlockEngine::isEnabled()) {
        qDebug() << "[WPE-BLOCKER] ad block disabled — discarding compiled filter from" << source;
        webkit_user_content_filter_unref(filter);
        return;
    }

    webkit_user_content_manager_add_filter(context->manager, filter);
    qDebug() << "[WPE-BLOCKER] installed content blocker from" << source
             << "id=" << webkit_user_content_filter_get_identifier(filter);
    webkit_user_content_filter_unref(filter);
}

void onContentBlockerSaved(GObject* sourceObject, GAsyncResult* result, gpointer userData)
{
    std::unique_ptr<ContentBlockerContext> context(static_cast<ContentBlockerContext*>(userData));
    GError* error = nullptr;
    WebKitUserContentFilter* filter =
        webkit_user_content_filter_store_save_from_file_finish(
            WEBKIT_USER_CONTENT_FILTER_STORE(sourceObject), result, &error);
    if (!filter) {
        qWarning() << "[WPE-BLOCKER] failed to compile content blocker:"
                   << (error ? error->message : "unknown error");
        g_clear_error(&error);
        return;
    }

    installContentBlockerFilter(context.get(), filter, "compile");
}

void onContentBlockerLoaded(GObject* sourceObject, GAsyncResult* result, gpointer userData)
{
    std::unique_ptr<ContentBlockerContext> context(static_cast<ContentBlockerContext*>(userData));
    GError* error = nullptr;
    WebKitUserContentFilter* filter =
        webkit_user_content_filter_store_load_finish(
            WEBKIT_USER_CONTENT_FILTER_STORE(sourceObject), result, &error);
    if (filter) {
        installContentBlockerFilter(context.get(), filter, "cache");
        return;
    }

    qDebug() << "[WPE-BLOCKER] cached content blocker unavailable, compiling from source:"
             << (error ? error->message : "missing filter");
    g_clear_error(&error);

    if (!context->store || !context->sourceFile) {
        qWarning() << "[WPE-BLOCKER] cannot compile content blocker: missing store or source file";
        return;
    }

    webkit_user_content_filter_store_save_from_file(
        context->store,
        context->identifier.constData(),
        context->sourceFile,
        nullptr,
        onContentBlockerSaved,
        context.release());
}

void ensureContentBlocker(WebKitUserContentManager* manager)
{
    if (!manager)
        return;

    const QString sourcePath = QString::fromUtf8(WPERuntimePaths::kAtlanticContentBlockerPath);
    if (!QFileInfo::exists(sourcePath)) {
        qDebug() << "[WPE-BLOCKER] source file missing:" << sourcePath;
        return;
    }

    // Build a versioned identifier: base + mtime so the compiled cache is
    // automatically invalidated whenever the JSON is updated.
    QFileInfo fi(sourcePath);
    const qint64 mtime = fi.lastModified().toMSecsSinceEpoch() / 1000;
    QByteArray identifier = QByteArray(kContentBlockerIdentifierBase)
                            + '-' + QByteArray::number((qlonglong)mtime);

    const QString storePath = contentBlockerStorePath();
    if (!QDir().mkpath(storePath)) {
        qWarning() << "[WPE-BLOCKER] failed to create filter store path:" << storePath;
        return;
    }

    auto context = std::make_unique<ContentBlockerContext>();
    context->manager = WEBKIT_USER_CONTENT_MANAGER(g_object_ref(manager));
    context->store = webkit_user_content_filter_store_new(storePath.toUtf8().constData());
    context->sourceFile = g_file_new_for_path(sourcePath.toUtf8().constData());
    context->identifier = identifier;

    webkit_user_content_filter_store_load(
        context->store,
        context->identifier.constData(),
        nullptr,
        onContentBlockerLoaded,
        context.release());
}

QString wellKnownPulseSocket()
{
    const QByteArray runtimeDir = qgetenv("XDG_RUNTIME_DIR").trimmed();
    if (runtimeDir.isEmpty()) {
        return {};
    }
    return QStringLiteral("unix:path=%1/pulse/dbus-socket")
        .arg(QString::fromLocal8Bit(runtimeDir));
}

QString pulseDbusAddress()
{
    const QByteArray envAddress = qgetenv("PULSE_DBUS_SERVER").trimmed();
    if (!envAddress.isEmpty()) {
        return QString::fromLocal8Bit(envAddress);
    }

    QDBusMessage request = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPulseLookupService),
        QString::fromLatin1(kPulseLookupPath),
        QString::fromLatin1(kPropertiesInterface),
        QStringLiteral("Get"));
    request << QString::fromLatin1(kPulseLookupInterface) << QStringLiteral("Address");

    const QDBusMessage reply = QDBusConnection::sessionBus().call(request);
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        const QVariant value = reply.arguments().constFirst();
        const QString address = value.canConvert<QDBusVariant>()
            ? qvariant_cast<QDBusVariant>(value).variant().toString()
            : value.toString();
        if (!address.isEmpty()) {
            return address;
        }
    }

    // org.pulseaudio.Server is not always registered on the session bus (seen
    // on SFOS 5.1.0.5 / Xperia 10 II), but the peer socket still lives at the
    // well-known path under XDG_RUNTIME_DIR. Fall back to it.
    return wellKnownPulseSocket();
}

// com.Meego.MainVolume2 is served by PulseAudio's module-dbus-protocol on the
// PulseAudio peer-to-peer D-Bus socket, NOT on the session bus. The peer socket
// answers org.freedesktop.DBus.Properties GetAll/Set with StepCount/CurrentStep.
//
// We use GDBus (GIO) rather than QtDBus for this peer connection: Qt 5.6.3's
// QDBusConnection::connectToPeer() does not complete the auth handshake against
// PulseAudio's socket (every GetAll failed → the volume slider never reached
// the page), whereas a GDBus peer connection — G_DBUS_CONNECTION_FLAGS_-
// AUTHENTICATION_CLIENT, no "Hello" — works, matching `dbus-send --peer`.
// GIO is already linked here for the WebKit GLib API.
//
// One cached connection is reused; it is dropped and reopened if PulseAudio
// restarted and the socket went away.
GDBusConnection* mainVolumePeerConnection()
{
    static GDBusConnection* cached = nullptr;
    if (cached) {
        if (!g_dbus_connection_is_closed(cached)) {
            return cached;
        }
        g_object_unref(cached);
        cached = nullptr;
    }

    const QString address = pulseDbusAddress();
    if (address.isEmpty()) {
        return nullptr;
    }

    GError* error = nullptr;
    cached = g_dbus_connection_new_for_address_sync(
        address.toUtf8().constData(),
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
        nullptr, nullptr, &error);
    if (!cached) {
        qWarning() << "[WPE-VOLUME] peer connect failed:"
                   << (error ? error->message : "unknown");
        g_clear_error(&error);
        return nullptr;
    }
    return cached;
}

MainVolumeState queryMainVolumeState()
{
    GDBusConnection* connection = mainVolumePeerConnection();
    if (!connection) {
        return {};
    }

    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        connection,
        nullptr, // peer connection: no destination bus name
        kMainVolumePath,
        kPropertiesInterface,
        "GetAll",
        g_variant_new("(s)", kMainVolumeInterface),
        G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        3000,
        nullptr,
        &error);
    if (!reply) {
        g_clear_error(&error);
        return {};
    }

    GVariant* properties = g_variant_get_child_value(reply, 0);
    GVariant* stepCountVar = g_variant_lookup_value(properties, "StepCount", G_VARIANT_TYPE_UINT32);
    GVariant* currentStepVar = g_variant_lookup_value(properties, "CurrentStep", G_VARIANT_TYPE_UINT32);

    MainVolumeState state;
    if (stepCountVar && currentStepVar) {
        const int stepCount = static_cast<int>(g_variant_get_uint32(stepCountVar));
        const int currentStep = static_cast<int>(g_variant_get_uint32(currentStepVar));
        if (stepCount > 0) {
            state.maximumStep = stepCount - 1;
            state.currentStep = qBound(0, currentStep, state.maximumStep);
        }
    }

    if (stepCountVar) {
        g_variant_unref(stepCountVar);
    }
    if (currentStepVar) {
        g_variant_unref(currentStepVar);
    }
    g_variant_unref(properties);
    g_variant_unref(reply);
    return state;
}

// NOTE: there is intentionally no setMainVolume* writer. Volume sync is
// read-only (system → el.volume); the browser never writes MainVolume2. A
// previous writer fed back through the poll and pinned the native slider.

void applyMediaVolumeToPage(WPEWebPage* page, qreal volume, bool postState)
{
    if (!page) {
        return;
    }

    page->runJavaScript(QString::fromLatin1(
        "(function(volume, postState){"
        "  window.__wpeDesiredMediaVolume = volume;"
        "  if (window.__wpeApplyDesiredMediaStateToMedia)"
        "    window.__wpeApplyDesiredMediaStateToMedia(document);"
        "  else {"
        "    var media = document.querySelectorAll('audio,video');"
        "    for (var i = 0; i < media.length; i++) {"
        "      media[i].volume = volume;"
        "    }"
        "  }"
        "  if (postState && window.__wpePostMediaState)"
        "    window.__wpePostMediaState();"
        "})(%1,%2);").arg(volume, 0, 'f', 3).arg(postState ? QStringLiteral("true") : QStringLiteral("false")));
}

bool envVarEnabled(const QByteArray &value)
{
    const QByteArray normalized = value.trimmed().toLower();
    return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

// ── User agent ───────────────────────────────────────────────────────────────
// Reworked after the engine switch: the gecko-era strings carried no Safari
// "Version/x" token, so version-sniffing sites could not classify the browser
// and fell back to their legacy/most-degraded code paths (one root cause of
// the YouTube icon trouble). Shape now mirrors what real WebKit browsers send:
//  - AppleWebKit/605.1.15 + Safari/605.1.15: frozen WebKit-family tokens.
//  - Version/26.0: Safari's feature-version token, matching the WebKit 2.52.x
//    era — this is what "is this a modern Safari?" sniffers read.
//  - Mobile ... Mobile Safari: standard mobile-detection tokens.
//  - SailfishOS in the platform comment slot: honest, ignored by sniffers.
//  - Atlantic/1.0 placed Chrome-style before "Mobile Safari" (the
//    "<Brand>/<ver> Mobile Safari/..." shape every UA parser handles).
// Desktop mode mirrors Safari's own "Request Desktop Website", which presents
// the macOS Safari UA — sites then serve the well-tested Mac variant instead
// of reacting to the rare "Linux + Safari" combination.
// ATLANTIC_USER_AGENT / ATLANTIC_USER_AGENT_DESKTOP override for testing
// without a rebuild.
QString atlanticUserAgent(bool desktopMode)
{
    const QByteArray envOverride =
        qgetenv(desktopMode ? "ATLANTIC_USER_AGENT_DESKTOP" : "ATLANTIC_USER_AGENT");
    if (!envOverride.trimmed().isEmpty())
        return QString::fromUtf8(envOverride.trimmed());

    if (desktopMode) {
        return QStringLiteral(
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
            "AppleWebKit/605.1.15 (KHTML, like Gecko) "
            "Version/26.0 Atlantic/1.0 Safari/605.1.15");
    }
    return QStringLiteral(
        "Mozilla/5.0 (Linux; Mobile; SailfishOS 5.1) "
        "AppleWebKit/605.1.15 (KHTML, like Gecko) "
        "Version/26.0 Atlantic/1.0 Mobile Safari/605.1.15");
}

bool perfLoggingEnabled()
{
    return envVarEnabled(qgetenv("ATLANTIC_PERF_LOG"));
}

int framePumpIntervalForCurrentGpuMode()
{
    // The pump is now a safety watchdog (default 2000 ms), not a 60 fps
    // compositor driver. Actual frame delivery is demand-driven: whenever
    // WPEQtViewBackend::displayImage() is called (i.e. WPE has new output),
    // it posts QQuickWindow::update() so the Qt render thread composites
    // exactly when there is something new to show, with no busy-idle GPU cost.
    // The watchdog only fires to unblock the render thread if it somehow stalls.
    bool ok = false;
    const int overrideMs = qEnvironmentVariableIntValue("ATLANTIC_FRAME_PUMP_INTERVAL_MS", &ok);
    if (ok && overrideMs >= 16 && overrideMs <= 10000) {
        return overrideMs;
    }
    return kDefaultFramePumpIntervalMs;
}

QStringList wildcardFiltersForFamily(const QString &family)
{
    if (family == QStringLiteral("image")) {
        return {
            QStringLiteral("*.png"),
            QStringLiteral("*.jpg"),
            QStringLiteral("*.jpeg"),
            QStringLiteral("*.gif"),
            QStringLiteral("*.bmp"),
            QStringLiteral("*.webp"),
            QStringLiteral("*.svg"),
            QStringLiteral("*.heif"),
            QStringLiteral("*.heic")
        };
    }
    if (family == QStringLiteral("audio")) {
        return {
            QStringLiteral("*.mp3"),
            QStringLiteral("*.aac"),
            QStringLiteral("*.m4a"),
            QStringLiteral("*.ogg"),
            QStringLiteral("*.wav"),
            QStringLiteral("*.flac")
        };
    }
    if (family == QStringLiteral("video")) {
        return {
            QStringLiteral("*.mp4"),
            QStringLiteral("*.m4v"),
            QStringLiteral("*.webm"),
            QStringLiteral("*.mkv"),
            QStringLiteral("*.mov"),
            QStringLiteral("*.avi"),
            QStringLiteral("*.3gp")
        };
    }
    if (family == QStringLiteral("text")) {
        return {
            QStringLiteral("*.txt"),
            QStringLiteral("*.md"),
            QStringLiteral("*.csv"),
            QStringLiteral("*.json"),
            QStringLiteral("*.xml"),
            QStringLiteral("*.html"),
            QStringLiteral("*.htm")
        };
    }
    return {};
}

QStringList mimeToFilters(const QString &mimeType)
{
    const QString mime = mimeType.trimmed().toLower();
    if (mime.isEmpty() || mime == QStringLiteral("*") || mime == QStringLiteral("*/*")) {
        return {};
    }

    const int slash = mime.indexOf(QLatin1Char('/'));
    if (slash <= 0 || slash >= mime.size() - 1) {
        return {};
    }

    const QString family = mime.left(slash);
    const QString subtype = mime.mid(slash + 1);

    if (subtype == QStringLiteral("*")) {
        return wildcardFiltersForFamily(family);
    }

    if (subtype == QStringLiteral("jpeg")) {
        return { QStringLiteral("*.jpg"), QStringLiteral("*.jpeg") };
    }
    if (subtype == QStringLiteral("svg+xml")) {
        return { QStringLiteral("*.svg") };
    }
    if (subtype == QStringLiteral("x-m4a")) {
        return { QStringLiteral("*.m4a") };
    }
    if (subtype == QStringLiteral("quicktime")) {
        return { QStringLiteral("*.mov") };
    }

    return { QStringLiteral("*.%1").arg(subtype) };
}

QStringList requestNameFilters(WebKitFileChooserRequest *request)
{
    if (!request) {
        return {};
    }

    const gchar* const* mimeTypes = webkit_file_chooser_request_get_mime_types(request);
    if (!mimeTypes || !mimeTypes[0]) {
        return {};
    }

    QSet<QString> filters;
    for (guint i = 0; mimeTypes[i]; ++i) {
        const QString mime = QString::fromUtf8(mimeTypes[i]).trimmed();
        if (mime.isEmpty() || mime == QStringLiteral("*") || mime == QStringLiteral("*/*")) {
            return {};
        }
        const QStringList mapped = mimeToFilters(mime);
        if (mapped.isEmpty()) {
            return {};
        }
        for (const QString &entry : mapped) {
            filters.insert(entry);
        }
    }

    QStringList ordered = filters.values();
    std::sort(ordered.begin(), ordered.end());
    return ordered;
}

QList<QTouchEvent::TouchPoint> mergeTrackedTouchPoints(
        QHash<int, QTouchEvent::TouchPoint> &trackedTouchPoints,
        const QList<QTouchEvent::TouchPoint> &touchPoints,
        QEvent::Type eventType)
{
    if (eventType == QEvent::TouchBegin && touchPoints.size() == 1) {
        trackedTouchPoints.clear();
    }

    for (const QTouchEvent::TouchPoint &touchPoint : touchPoints) {
        if (touchPoint.state() == Qt::TouchPointReleased) {
            trackedTouchPoints.remove(touchPoint.id());
        } else {
            trackedTouchPoints.insert(touchPoint.id(), touchPoint);
        }
    }

    if (eventType == QEvent::TouchCancel) {
        trackedTouchPoints.clear();
        return {};
    }

    QList<QTouchEvent::TouchPoint> activePoints = trackedTouchPoints.values();
    std::sort(activePoints.begin(), activePoints.end(), [](const QTouchEvent::TouchPoint &lhs, const QTouchEvent::TouchPoint &rhs) {
        return lhs.id() < rhs.id();
    });
    return activePoints;
}

void resetPinchZoomState(bool &pinchZoomActive, qreal &pinchStartDistance, double &pinchStartZoomLevel)
{
    pinchZoomActive = false;
    pinchStartDistance = 0.0;
    pinchStartZoomLevel = 1.0;
}

struct KeyboardProbeData {
    QPointer<WPEWebPage> page;
};

void onKeyboardProbeEvaluated(GObject* object, GAsyncResult* result, gpointer userData)
{
    std::unique_ptr<KeyboardProbeData> data(static_cast<KeyboardProbeData*>(userData));
    if (!data || !data->page)
        return;

    GError* error = nullptr;
    JSCValue* value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(object), result, &error);
    if (!value) {
        if (error) {
            g_error_free(error);
        }
        return;
    }

    bool editable = false;
    if (jsc_value_is_boolean(value)) {
        editable = jsc_value_to_boolean(value);
    }
    g_object_unref(value);

    QInputMethod* inputMethod = QGuiApplication::inputMethod();
    if (!inputMethod)
        return;

    if (editable) {
        inputMethod->show();
    } else if (inputMethod->isVisible()) {
        inputMethod->hide();
    }
}

gboolean onDownloadDecideDestination(WebKitDownload* download, gchar* suggestedFilename, gpointer)
{
    const QString suggested = suggestedFilename ? QString::fromUtf8(suggestedFilename) : QString();
    return DownloadManager::instance()->prepareDownload(download, suggested);
}

void onDownloadReceivedData(WebKitDownload* download, guint64, gpointer)
{
    DownloadManager::instance()->updateDownload(download);
}

void onDownloadFinished(WebKitDownload* download, gpointer)
{
    DownloadManager::instance()->downloadFinished(download);
}

void onDownloadFailed(WebKitDownload* download, GError* error, gpointer)
{
    const QString reason = error ? QString::fromUtf8(error->message) : QStringLiteral("Download failed");
    DownloadManager::instance()->downloadFailed(download, reason);
}

void onNetworkSessionDownloadStarted(WebKitNetworkSession*, WebKitDownload* download, gpointer)
{
    if (!download)
        return;

    g_signal_connect(download, "decide-destination", G_CALLBACK(onDownloadDecideDestination), nullptr);
    g_signal_connect(download, "received-data", G_CALLBACK(onDownloadReceivedData), nullptr);
    g_signal_connect(download, "finished", G_CALLBACK(onDownloadFinished), nullptr);
    g_signal_connect(download, "failed", G_CALLBACK(onDownloadFailed), nullptr);
}

gboolean onDecidePolicy(WebKitWebView* webView, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer)
{
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        WebKitResponsePolicyDecision* responseDecision =
            WEBKIT_RESPONSE_POLICY_DECISION(decision);
        if (!responseDecision ||
            webkit_response_policy_decision_is_main_frame_main_resource(responseDecision))
            return FALSE;

        WebKitURIRequest* request =
            webkit_response_policy_decision_get_request(responseDecision);
        const gchar* uri = request ? webkit_uri_request_get_uri(request) : nullptr;
        if (!uri || !*uri) return FALSE;

        QString sourceUrl;
        const gchar* activeUri = webkit_web_view_get_uri(webView);
        if (activeUri && *activeUri)
            sourceUrl = QString::fromUtf8(activeUri);
        else
            sourceUrl = QString::fromUtf8(uri);

        const char* rtype = "other";

        QString redirectUrl;
        if (AdBlockEngine::instance().shouldBlock(
                sourceUrl, QString::fromUtf8(uri), rtype, true, &redirectUrl)) {
            if (!redirectUrl.isEmpty()) {
                webkit_web_view_load_uri(webView, redirectUrl.toUtf8().constData());
            }
            webkit_policy_decision_ignore(decision);
            return TRUE;
        }
        return FALSE;
    }

    if (type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        return FALSE;
    }

    WebKitNavigationPolicyDecision* navigationDecision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
    WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(navigationDecision);
    WebKitURIRequest* request = action ? webkit_navigation_action_get_request(action) : nullptr;
    const gchar* uri = request ? webkit_uri_request_get_uri(request) : nullptr;
    if (uri && *uri) {
        webkit_web_view_load_uri(webView, uri);
    }
    webkit_policy_decision_ignore(decision);
    return TRUE;
}

static void onSelectBridgeMessage(WebKitUserContentManager*, JSCValue* value, gpointer userData)
{
    WPEWebPage* page = static_cast<WPEWebPage*>(userData);
    if (!page || !value)
        return;

    gchar* json = jsc_value_to_json(value, 0);
    if (!json)
        return;

    QByteArray jsonBytes(json);
    g_free(json);

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return;

    QJsonObject obj = doc.object();
    QJsonArray optArray = obj.value(QStringLiteral("options")).toArray();
    int selectedIndex = obj.value(QStringLiteral("selectedIndex")).toInt(0);

    QStringList items;
    items.reserve(optArray.size());
    for (const QJsonValue &v : optArray)
        items.append(v.toString());

    page->openSelectMenu(items, selectedIndex);
}

static void onSelectionBridgeMessage(WebKitUserContentManager*, JSCValue* value, gpointer userData)
{
    WPEWebPage* page = static_cast<WPEWebPage*>(userData);
    if (!page || !value)
        return;

    gchar* json = jsc_value_to_json(value, 0);
    if (!json)
        return;

    QByteArray jsonBytes(json);
    g_free(json);

    QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (!doc.isObject())
        return;

    const QJsonObject obj = doc.object();
    const QString type = obj.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("clear")) {
        page->handleJsSelectionClear();
    } else if (type == QLatin1String("select")) {
        page->handleJsSelectionUpdate(
            obj.value(QStringLiteral("text")).toString(),
            obj.value(QStringLiteral("sx")).toDouble(),
            obj.value(QStringLiteral("sy")).toDouble(),
            obj.value(QStringLiteral("ex")).toDouble(),
            obj.value(QStringLiteral("ey")).toDouble());
    }
}

static void onSelectionBridgeInstall(WebKitUserContentManager* ucm, WPEWebPage* page)
{
    g_signal_connect(ucm, "script-message-received::selectionBridge",
                     G_CALLBACK(onSelectionBridgeMessage), page);
    webkit_user_content_manager_register_script_message_handler(ucm, "selectionBridge", nullptr);

    static const gchar* selectionBridgeJs = R"JS(
(function() {
    if (window.__wpeSelectionBridgeInstalled) return;
    window.__wpeSelectionBridgeInstalled = true;

    function caretRect(node, offset) {
        if (!node)
            return null;
        var range = document.createRange();
        try {
            range.setStart(node, offset);
            range.setEnd(node, offset);
        } catch (e) {
            return null;
        }
        var rect = range.getBoundingClientRect();
        if (!rect)
            return null;
        return rect;
    }

    function pointRange(x, y) {
        if (document.caretRangeFromPoint)
            return document.caretRangeFromPoint(x, y);
        if (document.caretPositionFromPoint) {
            var p = document.caretPositionFromPoint(x, y);
            if (p) {
                var r = document.createRange();
                r.setStart(p.offsetNode, p.offset);
                r.collapse(true);
                return r;
            }
        }
        return null;
    }

    function selectWordAtPoint(x, y) {
        var sel = window.getSelection ? window.getSelection() : null;
        if (!sel)
            return false;

        var range = pointRange(x, y);
        if (!range)
            return false;

        var node = range.startContainer;
        var offset = range.startOffset;
        if (!node)
            return false;

        if (node.nodeType !== Node.TEXT_NODE) {
            if (node.childNodes && node.childNodes.length) {
                if (offset >= node.childNodes.length)
                    offset = node.childNodes.length - 1;
                if (offset < 0)
                    offset = 0;
                var child = node.childNodes[offset];
                if (child && child.nodeType === Node.TEXT_NODE) {
                    node = child;
                    offset = 0;
                }
            }
        }

        if (node.nodeType !== Node.TEXT_NODE)
            return false;

        var text = node.data || '';
        if (!text.length)
            return false;

        var start = offset;
        var end = offset;
        while (start > 0 && /\S/.test(text.charAt(start - 1)))
            start--;
        while (end < text.length && /\S/.test(text.charAt(end)))
            end++;

        if (start === end) {
            if (end < text.length)
                end++;
            else if (start > 0)
                start--;
        }

        try {
            var wordRange = document.createRange();
            wordRange.setStart(node, start);
            wordRange.setEnd(node, end);
            sel.removeAllRanges();
            sel.addRange(wordRange);
            return true;
        } catch (e) {
            return false;
        }
    }

    function selectionPayload() {
        var sel = window.getSelection ? window.getSelection() : null;
        if (!sel || !sel.rangeCount || sel.isCollapsed)
            return { type: 'clear' };

        var text = sel.toString();
        if (!text)
            return { type: 'clear' };

        var range = sel.getRangeAt(0);
        // Use range.start/end (order-invariant) instead of anchor/focus (drag-direction dependent).
        // Avoid getClientRects() — it forces synchronous layout on every selectionchange, which
        // is very expensive on heavy-DOM pages. Caret rects on collapsed ranges are much cheaper.
        var startRect = caretRect(range.startContainer, range.startOffset);
        var endRect = caretRect(range.endContainer, range.endOffset);
        var bounds = (!startRect || !endRect) ? range.getBoundingClientRect() : null;

        var sx = startRect ? startRect.left : (bounds ? bounds.left : 0);
        var sy = startRect ? startRect.bottom : (bounds ? bounds.bottom : 0);
        var ex = endRect ? endRect.right : (bounds ? bounds.right : 0);
        var ey = endRect ? endRect.bottom : (bounds ? bounds.bottom : 0);

        return {
            type: 'select',
            text: text,
            startX: sx,
            startY: sy,
            endX: ex,
            endY: ey,
            cursorX: ex,
            cursorY: ey,
            sx: sx,
            sy: sy,
            ex: ex,
            ey: ey
        };
    }

    var pendingSelectionRaf = null;

    function flushSelection() {
        try {
            window.webkit.messageHandlers.selectionBridge.postMessage(selectionPayload());
        } catch (ex) {
            console.error('[WPE-SEL-JS] postMessage error: ' + ex);
        }
    }

    // Throttle continuous selectionchange events to one IPC message per animation frame.
    function postSelection() {
        if (pendingSelectionRaf) return;
        pendingSelectionRaf = requestAnimationFrame(function() {
            pendingSelectionRaf = null;
            flushSelection();
        });
    }

    // For gesture-end events, cancel any pending frame and send immediately so the
    // final handle positions are never one frame late.
    function postSelectionFinal() {
        if (pendingSelectionRaf) {
            cancelAnimationFrame(pendingSelectionRaf);
            pendingSelectionRaf = null;
        }
        flushSelection();
    }

    var longPressTimer = null;
    var longPressPoint = null;
    var longPressStartPoint = null;
    var longPressMoveThreshold = 12;

    function cancelLongPress() {
        if (longPressTimer) {
            clearTimeout(longPressTimer);
            longPressTimer = null;
        }
        longPressPoint = null;
        longPressStartPoint = null;
    }

    function imageUrlAtPoint(x, y) {
        var el = document.elementFromPoint(x, y);
        while (el) {
            if (el.tagName && el.tagName.toLowerCase() === 'img') {
                var src = el.currentSrc || el.src || el.getAttribute('src');
                if (src) return src;
            }
            el = el.parentElement;
        }
        return null;
    }

    function beginLongPress(x, y) {
        cancelLongPress();
        longPressPoint = { x: x, y: y };
        longPressStartPoint = { x: x, y: y };
        longPressTimer = setTimeout(function() {
            longPressTimer = null;
            if (!longPressPoint) return;
            var lx = longPressPoint.x, ly = longPressPoint.y;
            longPressPoint = null;
            var imgUrl = imageUrlAtPoint(lx, ly);
            if (imgUrl) {
                try {
                    window.webkit.messageHandlers.imageLongPressBridge.postMessage(
                        { imageUrl: imgUrl, x: lx, y: ly });
                } catch(e) {}
                return;
            }
            if (selectWordAtPoint(lx, ly))
                postSelectionFinal();
        }, 350);
    }

    function touchPointFromEvent(e) {
        if (e.touches && e.touches.length)
            return e.touches[0];
        if (e.changedTouches && e.changedTouches.length)
            return e.changedTouches[0];
        return null;
    }

    document.addEventListener('selectionchange', postSelection, true);
    document.addEventListener('mouseup', postSelectionFinal, true);
    document.addEventListener('touchend', postSelectionFinal, {capture: true, passive: true});
    document.addEventListener('keyup', postSelectionFinal, true);
    document.addEventListener('contextmenu', function(e) {
        if (selectWordAtPoint(e.clientX, e.clientY)) {
            e.preventDefault();
            postSelectionFinal();
        }
    }, true);
    // passive:true lets WebKit compositor scroll without waiting for main-thread
    // confirmation that preventDefault() won't be called (none of these handlers call it)
    document.addEventListener('touchstart', function(e) {
        var p = touchPointFromEvent(e);
        if (p)
            beginLongPress(p.clientX, p.clientY);
    }, {capture: true, passive: true});
    document.addEventListener('touchmove', function(e) {
        var p = touchPointFromEvent(e);
        if (!p || !longPressStartPoint)
            return;
        var dx = p.clientX - longPressStartPoint.x;
        var dy = p.clientY - longPressStartPoint.y;
        if ((dx * dx) + (dy * dy) > longPressMoveThreshold * longPressMoveThreshold)
            cancelLongPress();
    }, {capture: true, passive: true});
    document.addEventListener('touchcancel', cancelLongPress, {capture: true, passive: true});
    document.addEventListener('touchend', cancelLongPress, {capture: true, passive: true});
})();
)JS";

    WebKitUserScript* script = webkit_user_script_new(
        selectionBridgeJs,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);

    // Disable backdrop-filter site-wide: it forces the GPU to composite a
    // blurred copy of every element behind the filtered layer, which is very
    // expensive on Adreno 610.  The visual effect (frosted-glass nav bars) is
    // a cosmetic nicety that costs too much on this hardware.
    // Also suppress the tap-highlight flash (blue overlay on long-press / tap
    // of links) — purely cosmetic removal of a visual delay.
    static const gchar* perfCssJs = R"JS(
(function() {
    if (document.getElementById('__wpe_perf_style')) return;
    var s = document.createElement('style');
    s.id = '__wpe_perf_style';
    s.textContent = '* { backdrop-filter: none !important; -webkit-backdrop-filter: none !important;'
                  + ' -webkit-tap-highlight-color: rgba(0,0,0,0) !important; }';
    (document.head || document.documentElement).appendChild(s);
})();
)JS";
    WebKitUserScript* perfScript = webkit_user_script_new(
        perfCssJs,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
        nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, perfScript);
    webkit_user_script_unref(perfScript);

    // Generic CSS-mask icon healer. Replaces the old YouTube-specific CSS
    // override (hardcoded English aria-label selectors + hand-drawn glyphs,
    // injected into EVERY site), which needed a manual patch for each newly
    // broken icon and could clobber legitimate "Share"/"Next"/"Comment"
    // elements on unrelated sites. Diagnosis recap: WPE 2.52.4 parses all
    // mask shorthands/longhands fine (verified against CSSProperties.json);
    // what actually broke was masked icons painting nothing — either the
    // EXTERNAL mask image (sprite) never became usable while inline data:
    // URI masks worked, or the icon had a mask but no background paint. So:
    //  1. Walk same-origin stylesheets once for rules whose mask-image is an
    //     external url(); fetch each unique URL and re-issue the rule with
    //     the image inlined as a data: URI (the form known to work). Real
    //     glyphs are preserved — nothing is hand-drawn, any language, any
    //     site, including icons that do not exist yet.
    //  2. For elements matched by mask rules that would paint nothing (mask
    //     present, fully transparent background, no background-image, no
    //     children/text), set background-color: currentColor — the color the
    //     site intends for masked icons.
    // Bounded cost: each stylesheet is walked once (deferred to idle); the
    // element pass only runs querySelectorAll over the handful of collected
    // mask selectors. Heals are logged with [WPE-ICON-HEAL] so future icon
    // breakage is diagnosable from the remote inspector instead of patched
    // blind. Set ATLANTIC_DISABLE_ICON_HEAL=1 to turn off.
    static const gchar* iconHealJs = R"JS(
(function() {
    if (window.__wpeIconHealInstalled) return;
    window.__wpeIconHealInstalled = true;

    var inlinePromises = {};   // absolute url -> Promise<data URI>
    var seenSheets = [];       // stylesheets already walked
    var maskSelectors = [];    // selectors of url()-mask rules
    var healedCount = 0;
    var styleEl = null;

    function overrideSheet() {
        if (!styleEl || !styleEl.parentNode) {
            styleEl = document.createElement('style');
            styleEl.id = '__wpe_icon_heal';
            (document.head || document.documentElement).appendChild(styleEl);
        }
        return styleEl.sheet;
    }

    function inlineUrl(abs) {
        if (!inlinePromises[abs]) {
            inlinePromises[abs] = fetch(abs, { credentials: 'omit' })
                .then(function(r) {
                    if (!r.ok) throw new Error('http ' + r.status);
                    return r.blob();
                })
                .then(function(blob) {
                    return new Promise(function(resolve, reject) {
                        var fr = new FileReader();
                        fr.onload = function() { resolve(fr.result); };
                        fr.onerror = reject;
                        fr.readAsDataURL(blob);
                    });
                });
        }
        return inlinePromises[abs];
    }

    function maskValueOf(style) {
        return style.getPropertyValue('-webkit-mask-image')
            || style.getPropertyValue('mask-image')
            || style.getPropertyValue('-webkit-mask')
            || style.getPropertyValue('mask')
            || '';
    }

    function externalUrlIn(value, baseHref) {
        if (value.indexOf('url(') === -1) return null;
        if (value.indexOf('data:') !== -1 || value.indexOf('gradient') !== -1) return null;
        var m = value.match(/url\((["']?)([^)"']+)\1\)/);
        if (!m) return null;
        var abs;
        try { abs = new URL(m[2], baseHref || document.baseURI).href; } catch (e) { return null; }
        return abs.indexOf('http') === 0 ? abs : null;
    }

    function walkRules(rules, baseHref) {
        for (var i = 0; i < rules.length; i++) {
            var rule = rules[i];
            if (rule.styleSheet) { walkSheet(rule.styleSheet); continue; }      // @import
            if (rule.cssRules) { walkRules(rule.cssRules, baseHref); continue; } // @media etc.
            if (!rule.style || !rule.selectorText) continue;
            var mv = maskValueOf(rule.style);
            if (!mv || mv.indexOf('url(') === -1 || mv.indexOf('gradient') !== -1) continue;
            var sel = rule.selectorText;
            if (maskSelectors.indexOf(sel) === -1) maskSelectors.push(sel);
            var abs = externalUrlIn(mv, baseHref);
            if (abs) {
                (function(sel2, abs2) {
                    inlineUrl(abs2).then(function(dataUri) {
                        var sheet = overrideSheet();
                        sheet.insertRule(sel2 + '{-webkit-mask-image:url("' + dataUri + '") !important;'
                                              + 'mask-image:url("' + dataUri + '") !important;}',
                                         sheet.cssRules.length);
                        healedCount++;
                        console.log('[WPE-ICON-HEAL] inlined external mask ' + abs2.slice(0, 120));
                    }).catch(function(e) {
                        console.log('[WPE-ICON-HEAL] cannot inline ' + abs2.slice(0, 120) + ': ' + e);
                    });
                })(sel, abs);
            }
            // Pseudo-elements cannot get inline styles; if a masked pseudo
            // declares no background paint at all, give it currentColor.
            if (/::?(before|after)/.test(sel)
                && !rule.style.getPropertyValue('background-color')
                && !rule.style.getPropertyValue('background')
                && !rule.style.getPropertyValue('background-image')) {
                try {
                    var s2 = overrideSheet();
                    s2.insertRule(sel + '{background-color:currentColor !important;}', s2.cssRules.length);
                } catch (e) {}
            }
        }
    }

    function walkSheet(sheet) {
        if (!sheet || seenSheets.indexOf(sheet) !== -1) return;
        seenSheets.push(sheet);
        var rules = null;
        try { rules = sheet.cssRules; } catch (e) { return; } // cross-origin
        if (rules) walkRules(rules, sheet.href);
    }

    function elementPass() {
        for (var i = 0; i < maskSelectors.length; i++) {
            var nodes;
            try { nodes = document.querySelectorAll(maskSelectors[i]); } catch (e) { continue; }
            for (var j = 0; j < nodes.length; j++) {
                var el = nodes[j];
                if (el.__wpeIconHealed || el.childElementCount > 0) continue;
                if (el.textContent && el.textContent.trim()) continue;
                var cs = getComputedStyle(el);
                var mi = cs.webkitMaskImage || cs.maskImage || 'none';
                if (mi.indexOf('url(') === -1) continue;
                if (cs.backgroundImage !== 'none') continue;
                var bg = cs.backgroundColor;
                if (bg !== 'transparent' && !/rgba\([^)]+,\s*0\)$/.test(bg)) continue;
                var rect = el.getBoundingClientRect();
                if (!rect.width || !rect.height || rect.width > 160 || rect.height > 160) continue;
                el.style.setProperty('background-color', 'currentColor', 'important');
                el.__wpeIconHealed = true;
                healedCount++;
            }
        }
        if (healedCount && healedCount !== window.__wpeIconHealLogged) {
            window.__wpeIconHealLogged = healedCount;
            console.log('[WPE-ICON-HEAL] healed ' + healedCount + ' icon(s) so far');
        }
    }

    var passPending = false;
    function pass() {
        if (passPending) return;
        passPending = true;
        var run = function() {
            passPending = false;
            try {
                var sheets = document.styleSheets;
                for (var i = 0; i < sheets.length; i++) walkSheet(sheets[i]);
                elementPass();
            } catch (e) {}
        };
        if (window.requestIdleCallback) requestIdleCallback(run, { timeout: 1000 });
        else setTimeout(run, 50);
    }

    document.addEventListener('DOMContentLoaded', pass, true);
    window.addEventListener('load', function() {
        pass();
        setTimeout(pass, 1500);
        setTimeout(pass, 5000);
    });
    // Late-loading <link rel=stylesheet> (capture phase sees their load events).
    document.addEventListener('load', function(e) {
        if (e.target && e.target.tagName === 'LINK') pass();
    }, true);
    // Player chrome / lazy UI appears on fullscreen or interaction.
    document.addEventListener('fullscreenchange', pass, true);
    document.addEventListener('webkitfullscreenchange', pass, true);
    var lastInteractionPass = 0;
    document.addEventListener('pointerup', function() {
        var now = Date.now();
        if (now - lastInteractionPass < 1000 || !maskSelectors.length) return;
        lastInteractionPass = now;
        pass();
    }, { capture: true, passive: true });
    pass();
})();
)JS";
    if (!envVarEnabled(qgetenv("ATLANTIC_DISABLE_ICON_HEAL"))) {
        WebKitUserScript* iconHealScript = webkit_user_script_new(
            iconHealJs,
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
            nullptr, nullptr);
        webkit_user_content_manager_add_script(ucm, iconHealScript);
        webkit_user_script_unref(iconHealScript);
    }

    // Always-smooth scrolling. JS-heavy pages — notably WordPress + Divi /
    // WPBakery (jolla.com) — attach NON-passive touch/wheel listeners at the
    // document level (sticky headers, parallax, scroll effects). On a touch
    // device that forces synchronous main-thread touch handling and disables
    // WebKit's compositor kinetic scroll, so a flick scrolls once with no
    // inertia and the user must re-gesture repeatedly ("stuck" scrolling).
    // Force document-level touchstart/touchmove/wheel/mousewheel listeners to be
    // passive so the page can't preventDefault() the scroll. This mirrors
    // Chrome's "passive by default for document-level touch listeners"
    // intervention, and is scoped to document-level targets (window/document/
    // html/body) so element-level sliders, carousels and <canvas> handlers that
    // legitimately need preventDefault keep working. Must inject at
    // DOCUMENT_START so the override is installed before the page's own scripts
    // bind their listeners. Set ATLANTIC_DISABLE_PASSIVE_SCROLL=1 to disable.
    if (!envVarEnabled(qgetenv("ATLANTIC_DISABLE_PASSIVE_SCROLL"))) {
        static const gchar* passiveScrollJs = R"JS(
(function() {
    try {
        var proto = EventTarget.prototype;
        if (proto.__wpePassivePatched) return;
        proto.__wpePassivePatched = true;
        var BLOCKERS = { touchstart: 1, touchmove: 1, wheel: 1, mousewheel: 1 };
        var orig = proto.addEventListener;
        proto.addEventListener = function(type, listener, options) {
            if (BLOCKERS[type] &&
                (this === window || this === document ||
                 this === document.documentElement || this === document.body)) {
                if (options === undefined || options === null || typeof options === 'boolean')
                    options = { capture: options === true, passive: true };
                else if (typeof options === 'object' && options.passive === undefined)
                    options = Object.assign({}, options, { passive: true });
            }
            return orig.call(this, type, listener, options);
        };
    } catch (e) {}
})();
)JS";
        WebKitUserScript* passiveScript = webkit_user_script_new(
            passiveScrollJs,
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
            nullptr, nullptr);
        webkit_user_content_manager_add_script(ucm, passiveScript);
        webkit_user_script_unref(passiveScript);
    }
}

static void onImageLongPressBridgeMessage(WebKitUserContentManager*, JSCValue* value, gpointer userData)
{
    WPEWebPage* page = static_cast<WPEWebPage*>(userData);
    if (!page || !value)
        return;

    gchar* json = jsc_value_to_json(value, 0);
    if (!json)
        return;

    QByteArray jsonBytes(json);
    g_free(json);

    QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (!doc.isObject())
        return;

    const QJsonObject obj = doc.object();
    const QString imageUrl = obj.value(QStringLiteral("imageUrl")).toString();
    const qreal x = obj.value(QStringLiteral("x")).toDouble();
    const qreal y = obj.value(QStringLiteral("y")).toDouble();
    if (imageUrl.isEmpty())
        return;

    QVariantMap data;
    data[QStringLiteral("imageUrl")] = imageUrl;
    data[QStringLiteral("x")] = x;
    data[QStringLiteral("y")] = y;
    emit page->recvAsyncMessage(QStringLiteral("Content:ImageLongPress"), data);
}

static void onImageLongPressBridgeInstall(WebKitUserContentManager* ucm, WPEWebPage* page)
{
    g_signal_connect(ucm, "script-message-received::imageLongPressBridge",
                     G_CALLBACK(onImageLongPressBridgeMessage), page);
    webkit_user_content_manager_register_script_message_handler(ucm, "imageLongPressBridge", nullptr);
}

static void onScrollBridgeMessage(WebKitUserContentManager*, JSCValue* value, gpointer userData)
{
    WPEWebPage* page = static_cast<WPEWebPage*>(userData);
    if (!page || !value)
        return;

    gchar* json = jsc_value_to_json(value, 0);
    if (!json)
        return;

    QByteArray jsonBytes(json);
    g_free(json);

    QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (!doc.isObject())
        return;

    const QJsonObject obj = doc.object();
    const qreal scrollY      = obj.value(QStringLiteral("scrollY")).toDouble();
    const qreal scrollHeight = obj.value(QStringLiteral("scrollHeight")).toDouble();
    const qreal innerHeight  = obj.value(QStringLiteral("innerHeight")).toDouble();
    emit page->scrollPositionChanged(scrollY, scrollHeight, innerHeight);
}

static void onScrollBridgeInstall(WebKitUserContentManager* ucm, WPEWebPage* page)
{
    g_signal_connect(ucm, "script-message-received::scrollBridge",
                     G_CALLBACK(onScrollBridgeMessage), page);
    webkit_user_content_manager_register_script_message_handler(ucm, "scrollBridge", nullptr);

    // Throttled scroll reporter: fires at most every 100 ms to avoid flooding
    // the native side. Reports scrollY, total scrollHeight, and innerHeight so
    // the chrome gesture logic in WPEWebPage can show/hide the toolbar.
    static const gchar* scrollBridgeJs = R"JS(
(function() {
    if (window.__wpeScrollBridgeInstalled) return;
    window.__wpeScrollBridgeInstalled = true;

    var pending = false;
    function report() {
        pending = false;
        try {
            window.webkit.messageHandlers.scrollBridge.postMessage({
                scrollY:      window.scrollY,
                scrollHeight: document.documentElement.scrollHeight,
                innerHeight:  window.innerHeight
            });
        } catch(e) {}
    }
    window.addEventListener('scroll', function() {
        if (!pending) {
            pending = true;
            setTimeout(report, 100);
        }
    }, {passive: true, capture: true});
})();
)JS";

    WebKitUserScript* script = webkit_user_script_new(
        scrollBridgeJs,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);
}

static void onMediaBridgeMessage(WebKitUserContentManager*, JSCValue* value, gpointer userData)
{
    WPEWebPage* page = static_cast<WPEWebPage*>(userData);
    if (!page || !value)
        return;

    gchar* json = jsc_value_to_json(value, 0);
    if (!json)
        return;

    QByteArray jsonBytes(json);
    g_free(json);

    QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (!doc.isObject())
        return;

    const QJsonObject obj = doc.object();
    const QString type = obj.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("clear")) {
        page->setFullscreenState(false);
        page->setMediaPlaybackState(false, false);
        return;
    }

    const bool videoActive = obj.value(QStringLiteral("videoActive")).toBool();
    const bool audioActive = obj.value(QStringLiteral("audioActive")).toBool() || videoActive;
    const bool fullscreenActive = obj.value(QStringLiteral("fullscreenActive")).toBool();
    const qreal volume = obj.value(QStringLiteral("volume")).toDouble(1.0);
    const bool muted = obj.value(QStringLiteral("muted")).toBool();
    const bool volumeChangedByPage = obj.value(QStringLiteral("volumeChangedByPage")).toBool();
    page->updateObservedMediaState(audioActive, videoActive, fullscreenActive, volume, muted, volumeChangedByPage);
}

static void onMediaBridgeInstall(WebKitUserContentManager* ucm, WPEWebPage* page)
{
    g_signal_connect(ucm, "script-message-received::mediaBridge",
                     G_CALLBACK(onMediaBridgeMessage), page);
    webkit_user_content_manager_register_script_message_handler(ucm, "mediaBridge", nullptr);

    static const gchar* mediaBridgeJs = R"JS(
(function() {
    if (window.__wpeMediaBridgeInstalled) return;
    window.__wpeMediaBridgeInstalled = true;
    var explicitFullscreenActive = false;
    var lastActiveTimestamp = 0;
    var lastTriggerEvent = null;

    function isMediaElement(el) {
        if (!el || !el.tagName)
            return false;
        var tag = el.tagName.toLowerCase();
        return tag === 'audio' || tag === 'video';
    }

    function detectFullscreenState() {
        if (document.fullscreenElement || document.webkitFullscreenElement)
            return true;

        var videos = document.querySelectorAll ? document.querySelectorAll('video') : [];
        for (var i = 0; i < videos.length; i++) {
            if (videos[i] && videos[i].webkitDisplayingFullscreen)
                return true;
        }
        return false;
    }

    function updateExplicitFullscreenState(event) {
        if (event) {
            if (event.type === 'webkitbeginfullscreen') {
                // Cancel any pending clear and mark fullscreen active.
                if (window.__wpeEndFullscreenTimer) {
                    clearTimeout(window.__wpeEndFullscreenTimer);
                    window.__wpeEndFullscreenTimer = null;
                }
                explicitFullscreenActive = true;
                return;
            }
            if (event.type === 'webkitendfullscreen') {
                // Intentionally ignore: webkitendfullscreen fires spuriously on WPE
                // during the viewport resize that accompanies fullscreen entry.
                // Real exits are signaled by C++ (leaveFullscreenRequested) injecting
                // window.__wpeClearExplicitFullscreen(), or by fullscreenchange.
                return;
            }
        }
        explicitFullscreenActive = detectFullscreenState();
    }

    // C++ (leaveFullscreenRequested) calls this to signal a real fullscreen exit.
    window.__wpeClearExplicitFullscreen = function() {
        if (window.__wpeEndFullscreenTimer) {
            clearTimeout(window.__wpeEndFullscreenTimer);
            window.__wpeEndFullscreenTimer = null;
        }
        explicitFullscreenActive = false;
        scheduleMediaState(null);
    };

    function normalizedDesiredVolume() {
        var volume = window.__wpeDesiredMediaVolume;
        if (typeof volume !== 'number' || !isFinite(volume))
            return null;
        return Math.max(0.0, Math.min(1.0, volume));
    }

    function applyDesiredMediaState(el) {
        if (!isMediaElement(el))
            return;

        var desiredVolume = normalizedDesiredVolume();
        if (desiredVolume !== null && el.volume !== desiredVolume)
            el.volume = desiredVolume;

        if (typeof window.__wpeDesiredMediaMuted === 'boolean' && el.muted !== window.__wpeDesiredMediaMuted)
            el.muted = window.__wpeDesiredMediaMuted;
    }

    function applyDesiredMediaStateToNode(node) {
        if (!node)
            return;

        if (isMediaElement(node))
            applyDesiredMediaState(node);

        if (!node.querySelectorAll)
            return;

        var media = node.querySelectorAll('audio,video');
        for (var i = 0; i < media.length; i++)
            applyDesiredMediaState(media[i]);
    }

    window.__wpeApplyDesiredMediaStateToMedia = applyDesiredMediaStateToNode;

    function mediaStatePayload() {
        var media = document.querySelectorAll('audio,video');
        var audioActive = false;
        var videoActive = false;
        var fullscreenActive = explicitFullscreenActive || detectFullscreenState();
        var volume = 1.0;
        var muted = false;
        var preferredMedia = null;

        for (var i = 0; i < media.length; i++) {
            var el = media[i];
            if (!isMediaElement(el))
                continue;

            applyDesiredMediaState(el);
            if (!preferredMedia || (!el.paused && !el.ended))
                preferredMedia = el;
            if (!fullscreenActive && el.tagName.toLowerCase() === 'video') {
                fullscreenActive = !!el.webkitDisplayingFullscreen;
            }
            if (!el.paused && !el.ended) {
                if (el.tagName.toLowerCase() === 'video') {
                    videoActive = true;
                    audioActive = true;
                } else if (!el.muted) {
                    // A playing, non-muted <audio> counts as audio-active even at
                    // volume 0, so the native-volume poll keeps running and the
                    // slider can bring it back up. Gating this on volume > 0 let a
                    // slide-to-zero stop the poll and freeze the element silent
                    // (volume-up could never recover it). Video is already handled
                    // above regardless of volume.
                    audioActive = true;
                }
            }
        }

        if (preferredMedia && preferredMedia.volume !== undefined) {
            volume = preferredMedia.volume;
            muted = preferredMedia.muted;
        }

        var definitiveInactive = lastTriggerEvent === 'pause'
            || lastTriggerEvent === 'ended'
            || lastTriggerEvent === 'emptied';
        if (audioActive) {
            lastActiveTimestamp = Date.now();
        } else if (lastTriggerEvent === 'waiting'
                   || lastTriggerEvent === 'stall'
                   || lastTriggerEvent === 'stalled') {
            audioActive = true;
        } else if (!definitiveInactive && (Date.now() - lastActiveTimestamp) < 2000) {
            audioActive = true;
        }

        return {
            type: 'state',
            audioActive: audioActive,
            videoActive: videoActive,
            fullscreenActive: fullscreenActive,
            volume: volume,
            muted: muted,
            volumeChangedByPage: lastTriggerEvent === 'volumechange'
        };
    }

    // While media is active, re-post the state every 2 s so the native side
    // notices silent stops (a playing element removed from the DOM pauses
    // without firing any media event). Replaces the old document-wide
    // MutationObserver, which re-scanned the DOM and posted an IPC message on
    // every mutation batch — a constant tax on mutation-heavy SPAs.
    var activeKeepalive = null;
    function postMediaState() {
        var payload = null;
        try {
            payload = mediaStatePayload();
            window.webkit.messageHandlers.mediaBridge.postMessage(payload);
        } catch (ex) {
            console.error('[WPE-MEDIA-JS] postMessage error: ' + ex);
        }
        lastTriggerEvent = null;
        var active = !!(payload && (payload.audioActive || payload.videoActive));
        if (active && !activeKeepalive) {
            activeKeepalive = setInterval(postMediaState, 2000);
        } else if (!active && activeKeepalive) {
            clearInterval(activeKeepalive);
            activeKeepalive = null;
        }
    }

    window.__wpePostMediaState = postMediaState;

    var events = [
        'play',
        'playing',
        'pause',
        'ended',
        'emptied',
        'volumechange',
        'loadeddata',
        'loadedmetadata',
        'seeking',
        'seeked'
    ];

    function onMediaEvent(event) {
        // Media events don't bubble but ARE seen here in the capture phase,
        // for any media element — including ones added to the DOM after load.
        // Applying the desired volume/mute at the moment an element does
        // something audible replaces the old MutationObserver scan.
        if (event && isMediaElement(event.target))
            applyDesiredMediaState(event.target);
        scheduleMediaState(event);
    }

    for (var i = 0; i < events.length; i++) {
        document.addEventListener(events[i], onMediaEvent, true);
    }

    function onFullscreenEvent(event) {
        updateExplicitFullscreenState(event);
        scheduleMediaState(event);
    }

    document.addEventListener('fullscreenchange', onFullscreenEvent, true);
    document.addEventListener('webkitfullscreenchange', onFullscreenEvent, true);
    document.addEventListener('webkitbeginfullscreen', onFullscreenEvent, true);
    document.addEventListener('webkitendfullscreen', onFullscreenEvent, true);

    // Debounced media state posting: collapse rapid-fire DOM and media events into
    // a single async querySelectorAll + IPC round-trip per turn.
    var mediaBridgePending = false;
    function scheduleMediaState(event) {
        if (event && event.type)
            lastTriggerEvent = event.type;
        else if (!mediaBridgePending)
            lastTriggerEvent = null;
        if (!mediaBridgePending) {
            mediaBridgePending = true;
            setTimeout(function() {
                mediaBridgePending = false;
                postMediaState();
            }, 0);
        }
    }

    // NOTE: no MutationObserver here on purpose. The previous implementation
    // observed {childList, subtree} on the whole document in every frame and,
    // per mutation batch, queued one setTimeout per added node (each running
    // querySelectorAll) plus a full document scan + mediaBridge IPC post. On
    // mutation-heavy pages (React/Vue SPAs, infinite feeds) that ran thousands
    // of times during load and continuously afterwards. New media elements are
    // instead caught by the capture-phase media event listeners above the
    // moment they load/play, and silent removals by the active keepalive.
    updateExplicitFullscreenState();
    document.addEventListener('DOMContentLoaded', postMediaState, true);
    applyDesiredMediaStateToNode(document);
    postMediaState();
})();
)JS";

    WebKitUserScript* script = webkit_user_script_new(
        mediaBridgeJs,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);
}

bool dispatchTextToFocusedElement(WPEWebPage* page, const QString& text, int replaceBeforeCaret)
{
    if (!page)
        return false;

    if (replaceBeforeCaret < 0)
        replaceBeforeCaret = 0;

    const QString args = QString::fromUtf8(QJsonDocument(QJsonArray{ text, replaceBeforeCaret }).toJson(QJsonDocument::Compact));
    const QString script = QStringLiteral(
        "(function(args){"
        "  var t=args[0];"
        "  var replaceBefore=(args[1]||0)|0;"
        "  function isEditable(e){"
        "    if(!e) return false;"
        "    if(e.isContentEditable) return true;"
        "    var tag=(e.tagName||'').toLowerCase();"
        "    if(tag==='textarea') return !e.readOnly && !e.disabled;"
        "    if(tag==='input'){"
        "      var tp=(e.type||'text').toLowerCase();"
        "      var blocked={button:1,submit:1,reset:1,checkbox:1,radio:1,file:1,image:1,range:1,color:1,hidden:1};"
        "      return !blocked[tp] && !e.readOnly && !e.disabled;"
        "    }"
        "    return false;"
        "  }"
        "  function fireInput(el,data,inputType){"
        "    try {"
        "      if (typeof InputEvent === 'function')"
        "        el.dispatchEvent(new InputEvent('input',{bubbles:true,data:data,inputType:inputType}));"
        "      else"
        "        el.dispatchEvent(new Event('input',{bubbles:true}));"
        "    } catch(_e) {"
        "      el.dispatchEvent(new Event('input',{bubbles:true}));"
        "    }"
        "  }"
        "  var el=document.activeElement;"
        "  if(!isEditable(el)) return false;"
        "  if(el.isContentEditable){"
        "    var sel=window.getSelection();"
        "    if(!sel || !sel.rangeCount) return false;"
        "    if(document.queryCommandSupported && document.queryCommandSupported('insertText')){"
        "      if(replaceBefore>0){"
        "        for(var i=0;i<replaceBefore;i++) document.execCommand('delete', false, null);"
        "      }"
        "      if(t.length) document.execCommand('insertText', false, t);"
        "      return true;"
        "    }"
        "    var r=sel.getRangeAt(0);"
        "    if(r.collapsed && replaceBefore>0){"
        "      var available = Math.min(replaceBefore, r.startOffset);"
        "      if(available>0) r.setStart(r.startContainer, r.startOffset-available);"
        "    }"
        "    r.deleteContents();"
        "    if(t.length){"
        "      var node=document.createTextNode(t);"
        "      r.insertNode(node);"
        "      r.setStartAfter(node);"
        "    }"
        "    r.collapse(true);"
        "    sel.removeAllRanges();"
        "    sel.addRange(r);"
        "    fireInput(el,t, t.length ? 'insertText' : 'deleteContentBackward');"
        "    return true;"
        "  }"
        "  var start=el.selectionStart;"
        "  var end=el.selectionEnd;"
        "  if(start===end && replaceBefore>0) start=Math.max(0,start-replaceBefore);"
        "  if(typeof start==='number' && typeof end==='number' && typeof el.setRangeText==='function'){"
        "    el.setRangeText(t,start,end,'end');"
        "  } else if(typeof start==='number' && typeof end==='number'){"
        "    var v=el.value||'';"
        "    el.value=v.slice(0,start)+t+v.slice(end);"
        "    var p=start+t.length;"
        "    if(el.setSelectionRange) el.setSelectionRange(p,p);"
        "  } else {"
        "    var vv=(el.value||'');"
        "    if(replaceBefore>0 && vv.length) vv=vv.slice(0, Math.max(0, vv.length-replaceBefore));"
        "    el.value=vv+t;"
        "  }"
        "  fireInput(el,t, t.length ? 'insertText' : 'deleteContentBackward');"
        "  return true;"
        "})(%1);")
        .arg(args);
    page->runJavaScript(script);
    if (QQuickWindow* win = page->window())
        win->update();
    page->update();
    QTimer::singleShot(16, page, [page]() {
        if (QQuickWindow* win = page->window())
            win->update();
        page->update();
    });
    return true;
}

bool dispatchBackspaceToFocusedElement(WPEWebPage* page)
{
    if (!page)
        return false;

    page->runJavaScript(QStringLiteral(
        "(function(){"
        "  function isEditable(e){"
        "    if(!e) return false;"
        "    if(e.isContentEditable) return true;"
        "    var tag=(e.tagName||'').toLowerCase();"
        "    if(tag==='textarea') return !e.readOnly && !e.disabled;"
        "    if(tag==='input'){"
        "      var tp=(e.type||'text').toLowerCase();"
        "      var blocked={button:1,submit:1,reset:1,checkbox:1,radio:1,file:1,image:1,range:1,color:1,hidden:1};"
        "      return !blocked[tp] && !e.readOnly && !e.disabled;"
        "    }"
        "    return false;"
        "  }"
        "  function fireInput(el){"
        "    try {"
        "      if (typeof InputEvent === 'function')"
        "        el.dispatchEvent(new InputEvent('input',{bubbles:true,inputType:'deleteContentBackward'}));"
        "      else"
        "        el.dispatchEvent(new Event('input',{bubbles:true}));"
        "    } catch(_e) {"
        "      el.dispatchEvent(new Event('input',{bubbles:true}));"
        "    }"
        "  }"
        "  var el=document.activeElement;"
        "  if(!isEditable(el)) return false;"
        "  if(el.isContentEditable){"
        "    var sel=window.getSelection();"
        "    if(!sel || !sel.rangeCount) return false;"
        "    if(document.queryCommandSupported && document.queryCommandSupported('delete')){"
        "      document.execCommand('delete', false, null);"
        "      return true;"
        "    }"
        "    var r=sel.getRangeAt(0);"
        "    if(r.collapsed){"
        "      if(r.startOffset<=0) return true;"
        "      r.setStart(r.startContainer,r.startOffset-1);"
        "    }"
        "    r.deleteContents();"
        "    sel.removeAllRanges();"
        "    sel.addRange(r);"
        "    fireInput(el);"
        "    return true;"
        "  }"
        "  var start=el.selectionStart;"
        "  var end=el.selectionEnd;"
        "  var v=el.value||'';"
        "  if(typeof start==='number' && typeof end==='number'){"
        "    if(start!==end){"
        "      el.value=v.slice(0,start)+v.slice(end);"
        "    } else if(start>0){"
        "      el.value=v.slice(0,start-1)+v.slice(end);"
        "      start-=1;"
        "    }"
        "    if(el.setSelectionRange) el.setSelectionRange(start,start);"
        "  } else if(v.length){"
        "    el.value=v.slice(0,-1);"
        "  }"
        "  fireInput(el);"
        "  return true;"
        "})();"));
    if (QQuickWindow* win = page->window())
        win->update();
    page->update();
    QTimer::singleShot(16, page, [page]() {
        if (QQuickWindow* win = page->window())
            win->update();
        page->update();
    });
    return true;
}

bool startSelectionAtPoint(WPEWebPage* page, qreal x, qreal y)
{
    if (!page)
        return false;

    const QString script = QStringLiteral(
        "(function(x,y){"
        "  function pointRange(){"
        "    if (document.caretRangeFromPoint)"
        "      return document.caretRangeFromPoint(x,y);"
        "    if (document.caretPositionFromPoint) {"
        "      var p = document.caretPositionFromPoint(x,y);"
        "      if (p) {"
        "        var r = document.createRange();"
        "        r.setStart(p.offsetNode, p.offset);"
        "        r.collapse(true);"
        "        return r;"
        "      }"
        "    }"
        "    return null;"
        "  }"
        "  function isWordChar(ch) { return /\\S/.test(ch); }"
        "  var sel = window.getSelection && window.getSelection();"
        "  if (!sel) return false;"
        "  var range = pointRange();"
        "  if (!range) return false;"
        "  var node = range.startContainer;"
        "  var offset = range.startOffset;"
        "  if (!node) return false;"
        "  if (node.nodeType !== Node.TEXT_NODE) {"
        "    if (node.childNodes && node.childNodes.length) {"
        "      if (offset >= node.childNodes.length) offset = node.childNodes.length - 1;"
        "      if (offset < 0) offset = 0;"
        "      var child = node.childNodes[offset];"
        "      if (child && child.nodeType === Node.TEXT_NODE) {"
        "        node = child;"
        "        offset = 0;"
        "      }"
        "    }"
        "  }"
        "  if (node.nodeType !== Node.TEXT_NODE) return false;"
        "  var text = node.data || '';"
        "  if (!text.length) return false;"
        "  var start = offset;"
        "  var end = offset;"
        "  while (start > 0 && isWordChar(text.charAt(start - 1))) start--;"
        "  while (end < text.length && isWordChar(text.charAt(end))) end++;"
        "  if (start === end) {"
        "    if (end < text.length) end++;"
        "    else if (start > 0) start--;"
        "  }"
        "  try {"
        "    var wordRange = document.createRange();"
        "    wordRange.setStart(node, start);"
        "    wordRange.setEnd(node, end);"
        "    sel.removeAllRanges();"
        "    sel.addRange(wordRange);"
        "    return true;"
        "  } catch (e) {"
        "    return false;"
        "  }"
        "})(%1,%2);")
        .arg(x, 0, 'f', 2)
        .arg(y, 0, 'f', 2);

    page->runJavaScript(script);
    if (QQuickWindow* win = page->window())
        win->update();
    page->update();
    QTimer::singleShot(16, page, [page]() {
        if (QQuickWindow* win = page->window())
            win->update();
        page->update();
    });
    return true;
}

bool shouldInterceptSoftKeyboardEvent(const QKeyEvent* event)
{
    if (!event)
        return false;
    QInputMethod* inputMethod = QGuiApplication::inputMethod();
    if (!inputMethod || !inputMethod->isVisible())
        return false;
    return !event->text().isEmpty() || event->key() == Qt::Key_Backspace;
}

} // namespace

WPEWebPage::WPEWebPage(QQuickItem *parent)
    : WPEQtView(parent)
    , m_security(new WPESecurityInfo(this))
{
    setFlag(QQuickItem::ItemAcceptsInputMethod, true);
    // This is a touch-only device. Hover events are meaningless and harmful:
    // they activate CSS :hover on links during scroll and interrupt gesture
    // recognition. Disable permanently. WebKit still synthesises a sticky hover
    // from taps (touchstart+touchend), so CSS :hover menus still work on tap.
    setAcceptHoverEvents(false);

    const int framePumpIntervalMs = framePumpIntervalForCurrentGpuMode();
    m_framePump.setInterval(framePumpIntervalMs);
    m_framePump.setTimerType(Qt::PreciseTimer);
    qDebug() << "[WPE] Frame pump interval" << framePumpIntervalMs
             << "ATLANTIC_GPU_CONSERVATIVE=" << qgetenv("ATLANTIC_GPU_CONSERVATIVE")
             << "ATLANTIC_PERF_LOG=" << qgetenv("ATLANTIC_PERF_LOG");
    connect(&m_framePump, &QTimer::timeout, this, [this]() {
        // Safety watchdog only. Real frame delivery is now demand-driven:
        // WPEQtViewBackend::displayImage() posts QQuickWindow::update() every
        // time WPE produces output, so the Qt render thread only composites
        // when there is actually something new to show. This watchdog fires
        // every 2 s as a last resort to prevent the render thread from
        // stalling permanently in rare edge cases.
        if (isVisible()) {
            if (QQuickWindow *w = window())
                w->update();
        }
    });
    m_mediaInactiveDebounceTimer.setSingleShot(true);
    m_mediaInactiveDebounceTimer.setInterval(kMediaInactiveDebounceMs);
    connect(&m_mediaInactiveDebounceTimer, &QTimer::timeout, this, [this]() {
        qDebug() << "[WPE-MEDIA] applying deferred inactive playback state";
        setMediaPlaybackState(false, false);
    });
    m_deferredFullscreenLeaveTimer.setSingleShot(true);
    m_deferredFullscreenLeaveTimer.setInterval(1200);
    connect(&m_deferredFullscreenLeaveTimer, &QTimer::timeout, this, [this]() {
        qDebug() << "[WPE-FULLSCREEN] applying deferred native leave";
        setNativeFullscreenRequested(false);
    });
    m_pendingFullscreenEntryGuard.setSingleShot(true);
    m_pendingFullscreenEntryGuard.setInterval(3000); // 3s guard window
    connect(&m_pendingFullscreenEntryGuard, &QTimer::timeout, this, [this]() {
        m_pendingFullscreenEntry = false;
        qDebug() << "[WPE-FULLSCREEN] pending entry guard expired";
    });
    m_fullscreenEnteredGuard.setSingleShot(true); // started by setDomFullscreenActive(true)

    // Volume poll timer: reads com.Meego.MainVolume2 on the session bus every 500ms
    // and applies the step as el.volume when the slider changes.
    m_volumePollTimer.setInterval(500);
    connect(&m_volumePollTimer, &QTimer::timeout, this, &WPEWebPage::pollMainVolume);

    setUserAgent(atlanticUserAgent(false));

    connect(this, &WPEQtView::loadingChanged,
            this, &WPEWebPage::onLoadingChanged);
    m_chromeGestureDebounceTimer.setSingleShot(true);
    m_chromeGestureDebounceTimer.setInterval(150);
    connect(&m_chromeGestureDebounceTimer, &QTimer::timeout, this, [this]() {
        if (m_chromeGestureArmed && m_chrome != m_pendingChrome) {
            setChrome(m_pendingChrome);
        }
        m_chromeGestureArmed = false;
    });

    connect(this, &WPEQtView::scrollPositionChanged,
            this, [this](qreal scrollY, qreal scrollHeight, qreal innerHeight) {
        bool atTop = scrollY <= 0;
        bool atBottom = (scrollHeight - scrollY - innerHeight) <= 1.0;
        if (atTop != m_atYBeginning) {
            m_atYBeginning = atTop;
            emit atYBeginningChanged();
        }
        if (atBottom != m_atYEnd) {
            m_atYEnd = atBottom;
            emit atYEndChanged();
        }
        if (m_chromeGestureEnabled && !m_fixedToolbar && !m_forcedChrome) {
            qreal delta = scrollY - m_lastScrollY;
            bool targetChrome;
            if (atTop) {
                targetChrome = true;
            } else if (delta > m_chromeGestureThreshold) {
                targetChrome = false;
            } else if (delta < -m_chromeGestureThreshold) {
                targetChrome = true;
            } else {
                m_lastScrollY = scrollY;
                return;
            }

            if (targetChrome != m_pendingChrome || !m_chromeGestureArmed) {
                m_pendingChrome = targetChrome;
                m_chromeGestureArmed = true;
                m_chromeGestureDebounceTimer.start();
            }
        }
        m_lastScrollY = scrollY;
    });
    connect(this, &WPEQtView::faviconUrlChanged,
            this, &WPEWebPage::setFavicon);
    connect(this, &WPEQtView::selectedTextChanged,
            this, [this](const QString& text) {
        bool wasActive = m_textSelectionActive;
        m_selectedText = text;
        m_textSelectionActive = !text.isEmpty();
        Q_EMIT selectionTextChanged();
        if (wasActive != m_textSelectionActive)
            Q_EMIT textSelectionActiveChanged();
        // Clear handle positions when selection is cleared
        if (!m_textSelectionActive) {
            m_selectionStartX = m_selectionStartY = m_selectionEndX = m_selectionEndY = 0.0;
            Q_EMIT selectionHandlesUpdated();
        }
    });

    connect(this, &WPEQtView::selectionHandlesChanged,
            this, [this](qreal sx, qreal sy, qreal ex, qreal ey) {
        m_selectionStartX = sx;
        m_selectionStartY = sy;
        m_selectionEndX = ex;
        m_selectionEndY = ey;
        Q_EMIT selectionHandlesUpdated();
    });

    connect(this, &WPEQtView::enterFullscreenRequested,
            this, [this]() {
        qDebug() << "[WPE-FULLSCREEN] native enter requested";
        m_lastNativeFullscreenEnter.restart();
        m_deferredFullscreenLeaveTimer.stop();
        m_pendingFullscreenEntry = true;
        m_pendingFullscreenEntryGuard.start();
        setNativeFullscreenRequested(true);
    });
    connect(this, &WPEQtView::leaveFullscreenRequested,
            this, [this]() {
        qDebug() << "[WPE-FULLSCREEN] native leave requested";
        // Always stop the entry guard: a real leave must be honoured.
        m_fullscreenEnteredGuard.stop();
        // Inject JS to clear explicitFullscreenActive so mediaBridge reports false.
        if (WebKitWebView* wv = webView()) {
            webkit_web_view_evaluate_javascript(
                    wv,
                    "window.__wpeClearExplicitFullscreen && window.__wpeClearExplicitFullscreen();",
                    -1, nullptr, nullptr, nullptr, nullptr, nullptr);
        }
        const bool enterWasRecent = m_lastNativeFullscreenEnter.isValid()
                && m_lastNativeFullscreenEnter.elapsed() < 2500;
        if (m_pendingFullscreenEntry || m_domFullscreenActive || enterWasRecent) {
            qDebug() << "[WPE-FULLSCREEN] deferring native leave"
                     << "pending=" << m_pendingFullscreenEntry
                     << "recentEnter=" << enterWasRecent
                     << "domFullscreen=" << m_domFullscreenActive;
            m_deferredFullscreenLeaveTimer.start();
            return;
        }
        setNativeFullscreenRequested(false);
    });
    connect(this, &WPEQtView::webViewCreated, this, [this]() {
        if (WebKitWebView* wv = webView()) {
            if (WebKitSettings* settings = webkit_web_view_get_settings(wv)) {
                webkit_settings_set_enable_fullscreen(settings, TRUE);
                // WPE always uses hardware acceleration (no GTK ON_DEMAND policy).
                // Enable WebGL and 2D canvas acceleration explicitly.
                webkit_settings_set_enable_webgl(settings, TRUE);
                webkit_settings_set_enable_2d_canvas_acceleration(settings, TRUE);
                // Disable smooth scrolling: on a touch device the WPE kinetic scroll
                // gesture recogniser already handles momentum. Enabling smooth scrolling
                // adds a second animation layer that makes pages feel rubber-banded
                // instead of tracking the finger 1:1.
                webkit_settings_set_enable_smooth_scrolling(settings, FALSE);
                // Require an explicit user gesture before any media element starts
                // playing. This prevents GStreamer pipelines from being initialized
                // in the background on page load (e.g. podcast cards on RadioFrance),
                // which would stall the WebProcess main thread and compete for CPU.
                webkit_settings_set_media_playback_requires_user_gesture(settings, TRUE);

                // FIX (hybris/Adreno blank page): keep DOM rendering in the
                // WebProcess. The engine is built with
                // ENABLE_GPU_PROCESS_DOM_RENDERING_BY_DEFAULT
                // (webkit-gpu-process-by-default-wpe.patch), but on this device the
                // GPU process cannot export composited frames — there is no GBM /
                // DRM render node (/dev/dri/renderD128 absent), only the libhybris
                // EGL fallback — so DOM-in-GPU rendering yields a blank content area
                // while the chrome still draws. Force the GPU-process DOM rendering
                // preference off so frames flow through the working WPEBackend-fdo
                // path. Set ATLANTIC_FORCE_GPU_DOM_RENDERING=1 to keep it on.
                if (!envVarEnabled(qgetenv("ATLANTIC_FORCE_GPU_DOM_RENDERING"))) {
                    if (WebKitFeatureList* allFeatures = webkit_settings_get_all_features()) {
                        for (gsize i = 0; i < webkit_feature_list_get_length(allFeatures); ++i) {
                            WebKitFeature* feature = webkit_feature_list_get(allFeatures, i);
                            if (g_strcmp0(webkit_feature_get_identifier(feature), "UseGPUProcessForDOMRenderingEnabled") == 0) {
                                webkit_settings_set_feature_enabled(settings, feature, FALSE);
                                qInfo("[Atlantic] GPU-process DOM rendering disabled (hybris frame-export workaround)");
                            }
                        }
                        webkit_feature_list_unref(allFeatures);
                    }
                }

                // Site isolation (experimental, opt-in via ATLANTIC_ENABLE_SITE_ISOLATION).
                // Puts cross-origin iframes / cross-site frames in separate WebProcesses;
                // each WebProcess is bwrap-confined, so this composes with the sandbox for
                // genuine cross-site isolation. The upstream feature is marked "unstable"
                // and multiplies the WebProcess count (heavy on a 3.5 GB device), so it is
                // OFF unless explicitly enabled. SiteIsolationSharedProcessEnabled bounds
                // the process count by sharing one process across cross-site frames.
                if (envVarEnabled(qgetenv("ATLANTIC_ENABLE_SITE_ISOLATION"))) {
                    if (WebKitFeatureList* features = webkit_settings_get_experimental_features()) {
                        for (gsize i = 0; i < webkit_feature_list_get_length(features); ++i) {
                            WebKitFeature* feature = webkit_feature_list_get(features, i);
                            const char* id = webkit_feature_get_identifier(feature);
                            if (g_strcmp0(id, "SiteIsolationEnabled") == 0
                                || g_strcmp0(id, "SiteIsolationSharedProcessEnabled") == 0)
                                webkit_settings_set_feature_enabled(settings, feature, TRUE);
                        }
                        webkit_feature_list_unref(features);
                    }
                    qInfo("[Atlantic] site isolation enabled (experimental, opt-in)");
                }
            }
            g_signal_connect(wv, "decide-policy", G_CALLBACK(onDecidePolicy), nullptr);
            {
                static bool engineInitialized = false;
                if (!engineInitialized) {
                    engineInitialized = true;
                    // Restore the persisted toggle BEFORE the first content
                    // blocker install. AdBlockEngine::s_enabled defaults to
                    // true, and the dconf binding in BrowserPage.qml only
                    // fires on *changes* — so without this read a disabled
                    // ad blocker came back on after every browser restart.
                    MDConfItem adBlockConf(QStringLiteral("/apps/atlantic-browser/settings/adblock_enabled"));
                    AdBlockEngine::setEnabled(adBlockConf.value(true).toBool());
                    qInfo() << "[ADBLOCK] enabled (persisted):" << AdBlockEngine::isEnabled();
                    QString cachePath = QStringLiteral("/usr/share/atlantic-browser/engine.dat");
                    if (!QFileInfo::exists(cachePath)) {
                        cachePath = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
                                    + QStringLiteral("/atlantic-browser/engine.dat");
                    }
                    if (!AdBlockEngine::instance().loadFromCache(cachePath)) {
                        qWarning() << "[ADBLOCK] engine not available — falling back to content blocker only";
                    }
                }
            }
            if (WebKitNetworkSession* session = webkit_web_view_get_network_session(wv)) {
                if (!g_object_get_data(G_OBJECT(session), "atlantic-download-hooked")) {
                    g_signal_connect(session, "download-started", G_CALLBACK(onNetworkSessionDownloadStarted), nullptr);
                    g_object_set_data(G_OBJECT(session), "atlantic-download-hooked", GINT_TO_POINTER(1));
                }
            }
            g_signal_connect(
                wv, "run-file-chooser",
                G_CALLBACK(+[](WebKitWebView*, WebKitFileChooserRequest* request, gpointer userData) -> gboolean {
                    auto *page = static_cast<WPEWebPage*>(userData);
                    return page && page->handleFileChooserRequest(request);
                }),
                this);

            // --- HTML <select> via JS bridge ---
            WebKitUserContentManager* ucm = webkit_web_view_get_user_content_manager(wv);
            // Always register (so a later enable can install the filter on
            // this tab), but only install when the toggle is on.
            registerContentBlockerManager(ucm);
            if (AdBlockEngine::isEnabled())
                ensureContentBlocker(ucm);

            // Tab-level crash isolation: catch WebProcess termination before it
            // propagates up and kills the UI, surface the crash state to QML,
            // and self-heal transient deaths with a single auto-reload.
            //
            // NOTE: the signal is "web-process-terminated" (a VOID__ENUM
            // notification carrying a WebKitWebProcessTerminationReason). The
            // old "web-process-crashed" name was deprecated/removed in WebKit
            // 2.20; connecting to it on 2.52 silently failed at runtime
            // (GLib-GObject-CRITICAL: signal invalid), so crash detection and
            // recovery never actually ran.
            g_signal_connect(
                wv, "web-process-terminated",
                G_CALLBACK(+[](WebKitWebView* view, WebKitWebProcessTerminationReason reason, gpointer userData) {
                    WPEWebPage* page = static_cast<WPEWebPage*>(userData);
                    qWarning("[Atlantic] WebProcess terminated (reason=%d) — recovering", int(reason));
                    if (!page->m_crashed) {
                        page->m_crashed = true;
                        Q_EMIT page->crashedChanged();
                    }
                    // One-shot auto-reload: heal a transient crash without user
                    // action. If it crashes again before a successful load
                    // clears m_autoRecovered, leave the crash UI up rather than
                    // looping a reload that keeps dying.
                    if (!page->m_autoRecovered) {
                        page->m_autoRecovered = true;
                        webkit_web_view_reload(view);
                    }
                }),
                this);
            // Connect BEFORE registering (per documentation, to avoid race conditions)
            g_signal_connect(ucm, "script-message-received::selectBridge",
                             G_CALLBACK(onSelectBridgeMessage), this);
            gboolean regOk = webkit_user_content_manager_register_script_message_handler(ucm, "selectBridge", nullptr);
            fprintf(stderr, "[WPE-SELECT] register_script_message_handler returned %d, ucm=%p\n", (int)regOk, ucm);

            // Inject JS: intercept <select> taps via multiple event types
            static const gchar* selectBridgeJs = R"JS(
(function() {
    if (window.__wpeSelectBridgeInstalled) return;
    window.__wpeSelectBridgeInstalled = true;
    console.error('[WPE-SELECT-JS] selectBridge JS installed, handlers=' +
        (window.webkit && window.webkit.messageHandlers ? 'YES' : 'NO'));

    function handleSelectActivation(e) {
        var el = e.target;
        while (el && el.tagName && el.tagName.toLowerCase() !== 'select') {
            el = el.parentElement;
        }
        if (!el || !el.tagName || el.tagName.toLowerCase() !== 'select') return;
        console.error('[WPE-SELECT-JS] intercepted ' + e.type + ' on <select>, options=' + el.options.length);
        e.preventDefault();
        e.stopImmediatePropagation();
        var opts = [];
        for (var i = 0; i < el.options.length; i++) {
            opts.push(el.options[i].text);
        }
        window.__wpePendingSelect = el;
        try {
            window.webkit.messageHandlers.selectBridge.postMessage({
                options: opts,
                selectedIndex: el.selectedIndex
            });
            console.error('[WPE-SELECT-JS] postMessage sent');
        } catch(ex) {
            console.error('[WPE-SELECT-JS] postMessage error: ' + ex);
        }
    }
    document.addEventListener('mousedown',   handleSelectActivation, true);
    document.addEventListener('touchstart',  handleSelectActivation, {capture: true, passive: true});
    document.addEventListener('pointerdown', handleSelectActivation, true);
    document.addEventListener('click',       handleSelectActivation, true);
    console.error('[WPE-SELECT-JS] event listeners registered');
})();
)JS";
            WebKitUserScript* script = webkit_user_script_new(
                selectBridgeJs,
                WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                nullptr, nullptr);
            webkit_user_content_manager_add_script(ucm, script);
            webkit_user_script_unref(script);

            onSelectionBridgeInstall(ucm, this);
            onImageLongPressBridgeInstall(ucm, this);
            onScrollBridgeInstall(ucm, this);
            onMediaBridgeInstall(ucm, this);

            WebKitNetworkSession* session = webkit_web_view_get_network_session(wv);
            if (!session) {
                session = webkit_network_session_get_default();
            }
            WebKitCookieManager* cookieManager = session ? webkit_network_session_get_cookie_manager(session) : nullptr;
            if (cookieManager) {
                const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
                const QString cookieDir = dataDir.isEmpty()
                    ? QStringLiteral("/home/defaultuser/.local/share/org.sailfishos/browser")
                    : dataDir;
                QDir().mkpath(cookieDir);
                const QString cookieFile = cookieDir + QStringLiteral("/cookies.sqlite");
                webkit_cookie_manager_set_persistent_storage(
                    cookieManager,
                    cookieFile.toUtf8().constData(),
                    WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
                webkit_cookie_manager_set_accept_policy(cookieManager, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
            }
        }
    });

    updateFramePumpState();
}

WPEWebPage::~WPEWebPage()
{
    clearFileChooserRequest(true);
}

int WPEWebPage::tabId() const { return m_tabId; }

void WPEWebPage::setTabId(int tabId)
{
    if (m_tabId != tabId) {
        m_tabId = tabId;
        emit tabIdChanged();
    }
}

bool WPEWebPage::painted() const { return m_painted; }
bool WPEWebPage::domContentLoaded() const { return m_domContentLoaded; }

bool WPEWebPage::chrome() const { return m_chrome; }

void WPEWebPage::setChrome(bool chrome)
{
    if (m_chrome != chrome) {
        m_chrome = chrome;
        emit chromeChanged();
    }
}

bool WPEWebPage::forcedChrome() const { return m_forcedChrome; }

void WPEWebPage::setForcedChrome(bool forcedChrome)
{
    if (m_forcedChrome != forcedChrome) {
        m_forcedChrome = forcedChrome;
        emit forcedChromeChanged();
    }
}

bool WPEWebPage::fullscreen() const { return m_fullscreen; }
bool WPEWebPage::atYBeginning() const { return m_atYBeginning; }
bool WPEWebPage::atYEnd() const { return m_atYEnd; }

QString WPEWebPage::favicon() const { return m_favicon; }

void WPEWebPage::setFavicon(const QString &favicon)
{
    if (m_favicon != favicon) {
        m_favicon = favicon;
        emit faviconChanged();
    }
}

qreal WPEWebPage::fullscreenHeight() const { return m_fullscreenHeight; }

void WPEWebPage::setFullscreenHeight(qreal height)
{
    if (!qFuzzyCompare(m_fullscreenHeight, height)) {
        m_fullscreenHeight = height;
        emit fullscreenHeightChanged();
    }
}

void WPEWebPage::syncEffectiveFullscreenState()
{
    const bool fullscreen = m_nativeFullscreenRequested || m_domFullscreenActive;
    if (m_fullscreen != fullscreen) {
        m_fullscreen = fullscreen;
        emit fullscreenChanged();
    }
}

void WPEWebPage::setFullscreenState(bool fullscreen)
{
    m_deferredFullscreenLeaveTimer.stop();
    const bool nativeChanged = (m_nativeFullscreenRequested != fullscreen);
    const bool domChanged = (m_domFullscreenActive != fullscreen);
    m_nativeFullscreenRequested = fullscreen;
    m_domFullscreenActive = fullscreen;
    if (nativeChanged || domChanged) {
        qDebug() << "[WPE-FULLSCREEN] forced state fullscreen=" << fullscreen;
    }
    syncEffectiveFullscreenState();
}

void WPEWebPage::setNativeFullscreenRequested(bool fullscreen)
{
    if (m_nativeFullscreenRequested == fullscreen) {
        return;
    }

    m_nativeFullscreenRequested = fullscreen;
    syncEffectiveFullscreenState();
}

void WPEWebPage::setDomFullscreenActive(bool fullscreen)
{
    if (m_domFullscreenActive == fullscreen) {
        return;
    }

    // Block spurious false reports while entry is pending OR within 3s of having
    // entered. WPE fires webkitendfullscreen transiently during the viewport resize
    // that accompanies native video fullscreen entry — both timers guard this window.
    if (!fullscreen && (m_pendingFullscreenEntry || m_fullscreenEnteredGuard.isActive())) {
        qDebug() << "[WPE-FULLSCREEN] blocking false report during entry/entered guard";
        return;
    }

    m_domFullscreenActive = fullscreen;
    if (fullscreen) {
        m_deferredFullscreenLeaveTimer.stop();
        m_pendingFullscreenEntry = false;
        m_pendingFullscreenEntryGuard.stop();
        // Start 3-second guard to suppress transient false reports after entering.
        m_fullscreenEnteredGuard.start(3000);
    } else {
        m_fullscreenEnteredGuard.stop();
    }
    qDebug() << "[WPE-FULLSCREEN] dom state fullscreen=" << fullscreen;
    syncEffectiveFullscreenState();
}

qreal WPEWebPage::selectionDisplayScale() const
{
    const qreal scale = currentPageZoomLevel();
    return scale > 0.0 ? scale : 1.0;
}

void WPEWebPage::pollMainVolume()
{
    const MainVolumeState state = queryMainVolumeState();
    if (!state.valid()) {
        qDebug() << "[WPE-VOLUME] poll: MainVolumeState invalid (peer socket unavailable?)";
        return;
    }
    if (state.currentStep == m_lastKnownVolumeStep) {
        return; // no change
    }
    m_lastKnownVolumeStep = state.currentStep;
    const qreal volume = (state.maximumStep > 0)
        ? qreal(state.currentStep) / qreal(state.maximumStep)
        : 1.0;
    qDebug() << "[WPE-VOLUME] poll: step=" << state.currentStep
             << "/" << state.maximumStep << "→ el.volume=" << volume;
    // Apply to page: set __wpeDesiredMediaVolume and trigger applyDesiredMediaState.
    // We do NOT set m_pageInitiatedVolumeChange so updateObservedMediaState won't
    // write back to the hardware slider (avoids round-trip).
    applyMediaVolumeToPage(this, volume, false);
}

void WPEWebPage::setMediaPlaybackState(bool audioActive, bool videoActive)
{
    if (m_mediaInactiveDebounceTimer.isActive())
        m_mediaInactiveDebounceTimer.stop();

    const bool changed = (m_mediaAudioActive != audioActive) || (m_mediaVideoActive != videoActive);
    if (m_mediaAudioActive != audioActive) {
        m_mediaAudioActive = audioActive;
        emit mediaAudioActiveChanged();
    }
    if (m_mediaVideoActive != videoActive) {
        m_mediaVideoActive = videoActive;
        emit mediaVideoActiveChanged();
    }

    if (!m_mediaAudioActive)
        m_pageInitiatedVolumeChange = false;

    // Start/stop volume poll timer based on audio activity.
    if (m_mediaAudioActive || m_mediaVideoActive) {
        if (!m_volumePollTimer.isActive()) {
            m_lastKnownVolumeStep = -1; // force immediate apply on first poll
            m_volumePollTimer.start();
            qDebug() << "[WPE-VOLUME] started poll timer";
            // Sync the system volume onto the page right away instead of
            // letting the media play at el.volume=1.0 (or a stale value) for
            // the first 500 ms poll interval.
            pollMainVolume();
        }
    } else {
        if (m_volumePollTimer.isActive()) {
            m_volumePollTimer.stop();
            qDebug() << "[WPE-VOLUME] stopped poll timer";
        }
    }

    if (changed) {
        qDebug() << "[WPE-MEDIA] playback state audio=" << m_mediaAudioActive
                 << "video=" << m_mediaVideoActive;
    }
}

void WPEWebPage::updateObservedMediaState(bool audioActive, bool videoActive, bool fullscreenActive,
                                          qreal volume, bool muted, bool volumeChangedByPage)
{
    if (audioActive || videoActive) {
        if (m_mediaInactiveDebounceTimer.isActive()) {
            m_mediaInactiveDebounceTimer.stop();
            qDebug() << "[WPE-MEDIA] cancelled deferred inactive playback state";
        }
        setMediaPlaybackState(audioActive, videoActive);
    } else if (m_mediaAudioActive || m_mediaVideoActive) {
        if (!m_mediaInactiveDebounceTimer.isActive()) {
            qDebug() << "[WPE-MEDIA] deferring inactive playback state";
            m_mediaInactiveDebounceTimer.start();
        }
    } else {
        if (m_mediaInactiveDebounceTimer.isActive())
            m_mediaInactiveDebounceTimer.stop();
        setMediaPlaybackState(false, false);
    }
    setDomFullscreenActive(fullscreenActive);

    if (volume < 0.0)
        volume = 0.0;
    if (volume > 1.0)
        volume = 1.0;

    // Volume sync is one-directional ONLY: system volume → el.volume (via the
    // poll timer). We deliberately do NOT write the page's volume back to the
    // system MainVolume2 here. Doing so (the old volumeChangedByPage path) fed
    // back on itself — the poll setting el.volume fires a 'volumechange', which
    // was reported as page-initiated and written back to the system volume,
    // pinning the native slider (and fighting in-page players). The browser now
    // only ever READS the system volume; the user owns it via the slider/keys.
    const bool volumeChanged = !qFuzzyCompare(m_mediaVolume + 1.0, volume + 1.0);
    const bool mutedChanged = (m_mediaMuted != muted);
    if (volumeChanged || mutedChanged) {
        m_mediaVolume = volume;
        m_mediaMuted = muted;
    }
}

qreal WPEWebPage::toolbarHeight() const { return m_toolbarHeight; }

void WPEWebPage::setToolbarHeight(qreal height)
{
    if (!qFuzzyCompare(m_toolbarHeight, height)) {
        m_toolbarHeight = height;
        emit toolbarHeightChanged();
    }
}

bool WPEWebPage::active() const { return m_active; }

void WPEWebPage::setActive(bool active)
{
    if (m_active != active) {
        m_active = active;
        setVisible(active);
        updateFramePumpState();
        emit activeChanged();
    }
}

bool WPEWebPage::throttlePainting() const { return m_throttlePainting; }

void WPEWebPage::setThrottlePainting(bool throttle)
{
    if (m_throttlePainting != throttle) {
        m_throttlePainting = throttle;
        updateFramePumpState();
        emit throttlePaintingChanged();
    }
}

bool WPEWebPage::chromeGestureEnabled() const { return m_chromeGestureEnabled; }

void WPEWebPage::setChromeGestureEnabled(bool enabled)
{
    if (m_chromeGestureEnabled != enabled) {
        m_chromeGestureEnabled = enabled;
        emit chromeGestureEnabledChanged();
    }
}

qreal WPEWebPage::chromeGestureThreshold() const { return m_chromeGestureThreshold; }

void WPEWebPage::setChromeGestureThreshold(qreal threshold)
{
    if (!qFuzzyCompare(m_chromeGestureThreshold, threshold)) {
        m_chromeGestureThreshold = threshold;
        emit chromeGestureThresholdChanged();
    }
}

bool WPEWebPage::fixedToolbar() const { return m_fixedToolbar; }

void WPEWebPage::setFixedToolbar(bool fixed)
{
    if (m_fixedToolbar != fixed) {
        m_fixedToolbar = fixed;
        emit fixedToolbarChanged();
    }
}

bool WPEWebPage::loaded() const { return m_loaded; }
bool WPEWebPage::moving() const { return false; }

QVariant WPEWebPage::resurrectedContentRect() const { return m_resurrectedContentRect; }

void WPEWebPage::setResurrectedContentRect(const QVariant &rect)
{
    m_resurrectedContentRect = rect;
    emit resurrectedContentRectChanged();
}

bool WPEWebPage::dragging() const { return false; }

bool WPEWebPage::desktopMode() const { return m_desktopMode; }

void WPEWebPage::setDesktopMode(bool desktop)
{
    const bool changed = (m_desktopMode != desktop);
    m_desktopMode = desktop;
    setUserAgent(atlanticUserAgent(desktop));
    if (changed) {
        emit desktopModeChanged();
        // Reload so the server sends the correct page variant.
        setUrl(url());
    }
}

void WPEWebPage::applyInitialDeviceScale(qreal scale)
{
    setDeviceScaleFactor(scale);
    rememberDefaultZoomLevel(scale);
    if (perfLoggingEnabled()) {
        qDebug() << "[WPE-PERF] initial scale applied"
                 << "deviceScaleFactor=" << scale
                 << "defaultZoom=" << m_defaultZoomLevel;
    }
}

double WPEWebPage::currentPageZoomLevel() const
{
    WebKitWebView *wv = webView();
    if (wv) {
        return webkit_web_view_get_zoom_level(wv);
    }
    if (m_defaultZoomLevelInitialized) {
        return m_defaultZoomLevel;
    }
    return 1.0;
}

void WPEWebPage::setPageZoomLevel(double zoomLevel)
{
    WebKitWebView *wv = webView();
    if (!wv) {
        return;
    }
    webkit_web_view_set_zoom_level(wv, zoomLevel);
}

void WPEWebPage::rememberDefaultZoomLevel(double zoomLevel)
{
    if (zoomLevel <= 0.0) {
        return;
    }
    m_defaultZoomLevel = zoomLevel;
    m_defaultZoomLevelInitialized = true;
}

double WPEWebPage::minimumPinchZoomLevel() const
{
    return m_defaultZoomLevelInitialized
        ? m_defaultZoomLevel * kMinimumPinchZoomFactor
        : kMinimumPinchZoomFactor;
}

double WPEWebPage::maximumPinchZoomLevel() const
{
    return m_defaultZoomLevelInitialized
        ? m_defaultZoomLevel * kMaximumPinchZoomFactor
        : kMaximumPinchZoomFactor;
}

void WPEWebPage::loadTab(const QString &url, bool force)
{
    QUrl newUrl(url);
    if (force || newUrl != this->url()) {
        setUrl(newUrl);
    }
}

// Convert a WebKitImage (BGRA8 premultiplied) to a scaled/cropped QImage.
// webkit_image_as_bytes() is transfer-none; we deep-copy before wkImage is freed.
static QImage snapshotToQImage(WebKitImage *wkImage, const QSize &targetSize)
{
    int w = webkit_image_get_width(wkImage);
    int h = webkit_image_get_height(wkImage);
    guint stride = webkit_image_get_stride(wkImage);
    GBytes *bytes = webkit_image_as_bytes(wkImage); // transfer none

    gsize dataSize = 0;
    const uchar *data = static_cast<const uchar *>(g_bytes_get_data(bytes, &dataSize));
    if (!data || w <= 0 || h <= 0 || dataSize < static_cast<gsize>(stride) * static_cast<gsize>(h))
        return QImage();

    // WebKitImage pixel format is BGRA8 premultiplied.
    // On little-endian ARM, QImage::Format_ARGB32_Premultiplied stores bytes as B,G,R,A — identical layout.
    QImage img(data, w, h, static_cast<int>(stride), QImage::Format_ARGB32_Premultiplied);
    img = img.copy(); // deep copy before wkImage is released by the caller

    if (targetSize.width() > 0 && img.width() != targetSize.width())
        img = img.scaledToWidth(targetSize.width(), Qt::SmoothTransformation);
    if (targetSize.height() > 0 && img.height() > targetSize.height())
        img = img.copy(0, 0, img.width(), targetSize.height());

    return img;
}

struct SnapshotFileData {
    QPointer<WPEWebPage> page;
    QString filePath;
    QSize targetSize;
};

struct SnapshotThumbnailData {
    QPointer<WPEWebPage> page;
    QSize targetSize;
};

static void onSnapshotFileReady(GObject *object, GAsyncResult *result, gpointer userData)
{
    std::unique_ptr<SnapshotFileData> ctx(static_cast<SnapshotFileData *>(userData));
    if (!ctx->page)
        return;

    GError *error = nullptr;
    WebKitImage *wkImage = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(object), result, &error);
    if (!wkImage) {
        if (error)
            g_error_free(error);
        return;
    }

    QImage img = snapshotToQImage(wkImage, ctx->targetSize);
    g_object_unref(wkImage);

    if (!img.isNull() && img.save(ctx->filePath, "PNG"))
        emit ctx->page->fileGrabWritten(ctx->filePath);
}

static void onSnapshotThumbnailReady(GObject *object, GAsyncResult *result, gpointer userData)
{
    std::unique_ptr<SnapshotThumbnailData> ctx(static_cast<SnapshotThumbnailData *>(userData));
    if (!ctx->page)
        return;

    GError *error = nullptr;
    WebKitImage *wkImage = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(object), result, &error);
    if (!wkImage) {
        if (error)
            g_error_free(error);
        return;
    }

    QImage img = snapshotToQImage(wkImage, ctx->targetSize);
    g_object_unref(wkImage);

    if (!img.isNull()) {
        QByteArray ba;
        QBuffer buf(&ba);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        buf.close();
        emit ctx->page->thumbnailResult(QString::fromLatin1(ba.toBase64()));
    }
}

void WPEWebPage::grabToFile(const QSize &size)
{
    WebKitWebView *wv = webView();
    if (!wv)
        return;

    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                       + QStringLiteral("/thumbnails");
    QDir().mkpath(cacheDir);
    const QString filePath = cacheDir + QStringLiteral("/") + QString::number(m_tabId) + QStringLiteral(".png");

    auto *ctx = new SnapshotFileData { this, filePath, size };
    webkit_web_view_get_snapshot(wv,
        WEBKIT_SNAPSHOT_REGION_VISIBLE,
        WEBKIT_SNAPSHOT_OPTIONS_NONE,
        nullptr,
        onSnapshotFileReady,
        ctx);
}

void WPEWebPage::grabThumbnail(const QSize &size)
{
    WebKitWebView *wv = webView();
    if (!wv)
        return;

    auto *ctx = new SnapshotThumbnailData { this, size };
    webkit_web_view_get_snapshot(wv,
        WEBKIT_SNAPSHOT_REGION_VISIBLE,
        WEBKIT_SNAPSHOT_OPTIONS_NONE,
        nullptr,
        onSnapshotThumbnailReady,
        ctx);
}

void WPEWebPage::forceChrome(bool forced)
{
    setForcedChrome(forced);
    if (forced) {
        setChrome(true);
    }
}

void WPEWebPage::suspendView()
{
    setVisible(false);
    updateFramePumpState();
    // Pause all media when suspending (e.g., app close)
    runJavaScript(QStringLiteral(
        "(function(){"
        "  var media = document.querySelectorAll('audio,video');"
        "  for (var i = 0; i < media.length; i++) {"
        "    if (!media[i].paused) media[i].pause();"
        "  }"
        "})();"));
}

void WPEWebPage::resumeView()
{
    setVisible(true);
    updateFramePumpState();
    update();
}

void WPEWebPage::sendAsyncMessage(const QString &name, const QVariant &data)
{
    if (name == QStringLiteral("Browser:SelectionStart")) {
        const QVariantMap map = data.toMap();
        const qreal x = map.value(QStringLiteral("xPos")).toReal();
        const qreal y = map.value(QStringLiteral("yPos")).toReal();
        startSelectionAtPoint(this, x, y);
        return;
    }

    if (name == QStringLiteral("embedui:exitFullscreen")) {
        runJavaScript(QStringLiteral(
            "(function(){"
            "  if (document.exitFullscreen && document.fullscreenElement) { document.exitFullscreen(); return; }"
            "  if (document.webkitExitFullscreen && document.webkitFullscreenElement) { document.webkitExitFullscreen(); return; }"
            "  var v = document.querySelector('video');"
            "  if (!v) return;"
            "  if (v.webkitExitFullscreen) v.webkitExitFullscreen();"
            "  else if (v.webkitCancelFullScreen) v.webkitCancelFullScreen();"
            "})();"));
        setFullscreenState(false);
    }
}

void WPEWebPage::setMediaVolume(qreal volume)
{
    if (volume > 1.0) volume /= 100.0;
    if (volume < 0.0) volume = 0.0;
    if (volume > 1.0) volume = 1.0;

    m_mediaVolume = volume;

    // Apply to the page only; never write the system MainVolume2 (read-only — see
    // updateObservedMediaState).
    qDebug() << "[WPE-MEDIA] set volume" << volume;
    applyMediaVolumeToPage(this, volume, true);
}

void WPEWebPage::setMediaMuted(bool muted)
{
    m_mediaMuted = muted;
    qDebug() << "[WPE-MEDIA] set muted" << muted;
    runJavaScript(QString::fromLatin1(
        "(function(muted){"
        "  window.__wpeDesiredMediaMuted = muted;"
        "  if (window.__wpeApplyDesiredMediaStateToMedia)"
        "    window.__wpeApplyDesiredMediaStateToMedia(document);"
        "  else {"
        "    var media = document.querySelectorAll('audio,video');"
        "    for (var i = 0; i < media.length; i++) {"
        "      media[i].muted = muted;"
        "    }"
        "  }"
        "  if (window.__wpePostMediaState)"
        "    window.__wpePostMediaState();"
        "})(%1);").arg(muted ? "true" : "false"));
}

void WPEWebPage::addMessageListener(const QString &)
{
    // stub
}

void WPEWebPage::clearSelection()
{
    handleJsSelectionClear();
    runJavaScript(QStringLiteral("window.getSelection().removeAllRanges();"));
}

void WPEWebPage::selectAll()
{
    runJavaScript(QStringLiteral("document.execCommand('selectAll');"));
}

void WPEWebPage::copyToClipboard()
{
    if (!m_selectedText.isEmpty())
        QGuiApplication::clipboard()->setText(m_selectedText);
}

void WPEWebPage::handleJsSelectionClear()
{
    const bool wasActive = m_textSelectionActive;
    const bool hadSelection = !m_selectedText.isEmpty() || m_selectionStartX > 0.0 || m_selectionEndX > 0.0;

    m_selectedText.clear();
    m_textSelectionActive = false;
    m_selectionStartX = m_selectionStartY = m_selectionEndX = m_selectionEndY = 0.0;

    if (hadSelection)
        Q_EMIT selectionTextChanged();
    if (wasActive)
        Q_EMIT textSelectionActiveChanged();
    if (hadSelection || wasActive)
        Q_EMIT selectionHandlesUpdated();
}

void WPEWebPage::handleJsSelectionUpdate(const QString &text, qreal startX, qreal startY, qreal endX, qreal endY)
{
    const qreal displayScale = selectionDisplayScale();
    const bool wasActive = m_textSelectionActive;
    const bool textChanged = (m_selectedText != text);
    const bool activeNow = !text.isEmpty();
    const qreal scaledStartX = startX * displayScale;
    const qreal scaledStartY = startY * displayScale;
    const qreal scaledEndX = endX * displayScale;
    const qreal scaledEndY = endY * displayScale;
    const bool handlesChanged = !qFuzzyCompare(m_selectionStartX, scaledStartX)
        || !qFuzzyCompare(m_selectionStartY, scaledStartY)
        || !qFuzzyCompare(m_selectionEndX, scaledEndX)
        || !qFuzzyCompare(m_selectionEndY, scaledEndY);

    m_selectedText = text;
    m_textSelectionActive = activeNow;
    m_selectionStartX = scaledStartX;
    m_selectionStartY = scaledStartY;
    m_selectionEndX = scaledEndX;
    m_selectionEndY = scaledEndY;

    if (textChanged)
        Q_EMIT selectionTextChanged();
    if (wasActive != activeNow)
        Q_EMIT textSelectionActiveChanged();
    if (textChanged || handlesChanged || wasActive != activeNow)
        Q_EMIT selectionHandlesUpdated();

    QVariantMap data;
    data.insert(QStringLiteral("text"), text);
    data.insert(QStringLiteral("startX"), scaledStartX);
    data.insert(QStringLiteral("startY"), scaledStartY);
    data.insert(QStringLiteral("endX"), scaledEndX);
    data.insert(QStringLiteral("endY"), scaledEndY);
    data.insert(QStringLiteral("cursorX"), scaledEndX);
    data.insert(QStringLiteral("cursorY"), scaledEndY);
    data.insert(QStringLiteral("sx"), scaledStartX);
    data.insert(QStringLiteral("sy"), scaledStartY);
    data.insert(QStringLiteral("ex"), scaledEndX);
    data.insert(QStringLiteral("ey"), scaledEndY);
    emit recvAsyncMessage(QStringLiteral("Content:SelectionRange"), data);
}

void WPEWebPage::moveSelectionStart(qreal cssX, qreal cssY)
{
    const qreal displayScale = selectionDisplayScale();
    if (displayScale > 0.0) {
        cssX /= displayScale;
        cssY /= displayScale;
    }

    QString js = QStringLiteral(
        "(function(x,y){"
        "  var sel=window.getSelection();"
        "  if(!sel.rangeCount)return;"
        "  var range=sel.getRangeAt(0);"
        "  var r=document.caretRangeFromPoint(x,y);"
        "  if(!r)return;"
        "  try{range.setStart(r.startContainer,r.startOffset);"
        "  sel.removeAllRanges();sel.addRange(range);}catch(e){}"
        "})(%1,%2);"
    ).arg(cssX, 0, 'f', 2).arg(cssY, 0, 'f', 2);
    runJavaScript(js);
}

void WPEWebPage::moveSelectionEnd(qreal cssX, qreal cssY)
{
    const qreal displayScale = selectionDisplayScale();
    if (displayScale > 0.0) {
        cssX /= displayScale;
        cssY /= displayScale;
    }

    QString js = QStringLiteral(
        "(function(x,y){"
        "  var sel=window.getSelection();"
        "  if(!sel.rangeCount)return;"
        "  var range=sel.getRangeAt(0);"
        "  var r=document.caretRangeFromPoint(x,y);"
        "  if(!r)return;"
        "  try{range.setEnd(r.startContainer,r.startOffset);"
        "  sel.removeAllRanges();sel.addRange(range);}catch(e){}"
        "})(%1,%2);"
    ).arg(cssX, 0, 'f', 2).arg(cssY, 0, 'f', 2);
    runJavaScript(js);
}

bool WPEWebPage::textSelectionActive() const { return m_textSelectionActive; }

QObject* WPEWebPage::textSelectionController() { return this; }

QString WPEWebPage::selectedText() const { return m_selectedText; }

bool WPEWebPage::isPhoneNumber() const
{
    static const QRegularExpression re(QStringLiteral("^[+\\d][\\d\\s\\-().+]{6,}$"));
    return re.match(m_selectedText.trimmed()).hasMatch();
}

QString WPEWebPage::searchUri() const
{
    return QStringLiteral("https://duckduckgo.com/?q=") +
           QString::fromUtf8(QUrl::toPercentEncoding(m_selectedText.trimmed()));
}

void WPEWebPage::onLoadingChanged(WPEQtViewLoadRequest *loadRequest)
{
    if (!loadRequest)
        return;

    switch (loadRequest->status()) {
    case WPEQtView::LoadStartedStatus:
        clearFileChooserRequest(true);
        m_domContentLoaded = false;
        m_loaded = false;
        m_favicon.clear();
        m_lastScrollY = 0.0;
        m_atYBeginning = true;
        m_atYEnd = false;
        if (m_crashed) {
            m_crashed = false;
            Q_EMIT crashedChanged();
        }
        if (m_textSelectionActive) {
            m_selectedText.clear();
            m_textSelectionActive = false;
            Q_EMIT selectionTextChanged();
            Q_EMIT textSelectionActiveChanged();
        }
        if (m_mediaInactiveDebounceTimer.isActive())
            m_mediaInactiveDebounceTimer.stop();
        setMediaPlaybackState(false, false);
        // Reset security on new navigation
        static_cast<WPESecurityInfo*>(m_security)->reset();
        emit securityChanged();
        // Reset visual pinch zoom so new page starts at 1:1
        if (!qFuzzyCompare(m_visualScale, 1.0)) {
            m_visualScale = 1.0;
        }
        setChrome(true);
        emit domContentLoadedChanged();
        emit loadedChanged();
        emit faviconChanged();
        break;

    case WPEQtView::LoadSucceededStatus:
        m_domContentLoaded = true;
        m_loaded = true;
        m_autoRecovered = false;
        updateSecurityInfo();
        emit domContentLoadedChanged();
        emit loadedChanged();
        if (AdBlockEngine::instance().isLoaded()) {
            QTimer::singleShot(300, this, [this]() {
                AdBlockEngine::instance().applyCosmetics(this);
            });
        }
        break;

    case WPEQtView::LoadStoppedStatus:
    case WPEQtView::LoadFailedStatus:
        m_loaded = false;
        setMediaPlaybackState(false, false);
        emit loadedChanged();
        break;
    }
}

bool WPEWebPage::handleFileChooserRequest(WebKitFileChooserRequest *request)
{
    if (!request) {
        return false;
    }

    clearFileChooserRequest(true);

    m_fileChooserRequest = WEBKIT_FILE_CHOOSER_REQUEST(g_object_ref(request));

    const bool selectMultiple = webkit_file_chooser_request_get_select_multiple(request);
    const gchar* const* mimeTypes = webkit_file_chooser_request_get_mime_types(request);
    QStringList requestedMimeTypes;
    if (mimeTypes) {
        for (guint i = 0; mimeTypes[i]; ++i) {
            requestedMimeTypes.append(QString::fromUtf8(mimeTypes[i]));
        }
    }
    fprintf(stderr, "[WPE-FILE] open request=%p selectMultiple=%d mimeTypes=%s\n",
            static_cast<void*>(request), selectMultiple ? 1 : 0,
            requestedMimeTypes.join(QStringLiteral(",")).toUtf8().constData());
    if (m_fileChooserSelectMultiple != selectMultiple) {
        m_fileChooserSelectMultiple = selectMultiple;
        emit fileChooserSelectMultipleChanged();
    }

    const QStringList filters = requestNameFilters(request);
    if (m_fileChooserNameFilters != filters) {
        m_fileChooserNameFilters = filters;
        emit fileChooserNameFiltersChanged();
    }

    if (!m_fileChooserActive) {
        m_fileChooserActive = true;
        emit fileChooserActiveChanged();
    }

    return true;
}

void WPEWebPage::clearFileChooserRequest(bool cancelRequest)
{
    fprintf(stderr, "[WPE-FILE] clear request=%p cancel=%d active=%d\n",
            static_cast<void*>(m_fileChooserRequest), cancelRequest ? 1 : 0, m_fileChooserActive ? 1 : 0);
    if (m_fileChooserRequest) {
        if (cancelRequest) {
            webkit_file_chooser_request_cancel(m_fileChooserRequest);
        }
        g_object_unref(m_fileChooserRequest);
        m_fileChooserRequest = nullptr;
    }

    if (m_fileChooserActive) {
        m_fileChooserActive = false;
        emit fileChooserActiveChanged();
    }
    if (!m_fileChooserNameFilters.isEmpty()) {
        m_fileChooserNameFilters.clear();
        emit fileChooserNameFiltersChanged();
    }
    if (m_fileChooserSelectMultiple) {
        m_fileChooserSelectMultiple = false;
        emit fileChooserSelectMultipleChanged();
    }
}

void WPEWebPage::itemChange(ItemChange change, const ItemChangeData &value)
{
    WPEQtView::itemChange(change, value);
    if (change == ItemSceneChange && value.window) {
        connect(value.window, &QQuickWindow::frameSwapped,
                this, &WPEWebPage::onFrameSwapped,
                Qt::UniqueConnection);
    } else if (change == ItemVisibleHasChanged) {
        updateFramePumpState();
    }
}

void WPEWebPage::updateFramePumpState()
{
    const bool shouldPump = isVisible();
    const int targetIntervalMs = framePumpIntervalForCurrentGpuMode();
    const bool intervalChanged = m_framePump.interval() != targetIntervalMs;
    if (intervalChanged) {
        m_framePump.setInterval(targetIntervalMs);
    }

    const bool wasActive = m_framePump.isActive();
    if (shouldPump) {
        if (!wasActive) {
            m_framePump.start();
        }
    } else if (wasActive) {
        m_framePump.stop();
    }

    if (perfLoggingEnabled() && (intervalChanged || wasActive != m_framePump.isActive())) {
        qDebug() << "[WPE-PERF] frame pump"
                 << (m_framePump.isActive() ? "running" : "stopped")
                 << "intervalMs=" << targetIntervalMs
                 << "visible=" << isVisible()
                 << "active=" << m_active
                 << "throttle=" << m_throttlePainting
                 << "video=" << m_mediaVideoActive
                 << "audio=" << m_mediaAudioActive;
    }
}

QVariant WPEWebPage::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImEnabled)
        return true;
    return QQuickItem::inputMethodQuery(query);
}

void WPEWebPage::inputMethodEvent(QInputMethodEvent *event)
{
    if (!event)
        return;

    bool handled = false;
    const QString committed = event->commitString();
    const QString preedit = event->preeditString();
    if (!committed.isEmpty()) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const bool duplicateOfRecentSoftKey =
            (committed == m_lastSoftKeyboardText) &&
            (nowMs - m_lastSoftKeyboardTextTimeMs >= 0) &&
            (nowMs - m_lastSoftKeyboardTextTimeMs < 200);

        if (!duplicateOfRecentSoftKey) {
            const int replaceBefore = m_lastPreeditText.size();
            handled = dispatchTextToFocusedElement(this, committed, replaceBefore) || handled;
        }

        m_lastSoftKeyboardText.clear();
        m_lastSoftKeyboardTextTimeMs = 0;
        m_lastPreeditText.clear();
    } else if (!preedit.isEmpty()) {
        handled = dispatchTextToFocusedElement(this, preedit, m_lastPreeditText.size()) || handled;
        m_lastPreeditText = preedit;
    } else if (!m_lastPreeditText.isEmpty()) {
        handled = dispatchTextToFocusedElement(this, QString(), m_lastPreeditText.size()) || handled;
        m_lastPreeditText.clear();
    }

    const int deleteCount = event->replacementLength();
    if (deleteCount > 0) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const bool duplicateOfRecentSoftBackspace =
            (nowMs - m_lastSoftBackspaceTimeMs >= 0) &&
            (nowMs - m_lastSoftBackspaceTimeMs < 200);
        if (!duplicateOfRecentSoftBackspace) {
            for (int i = 0; i < deleteCount; ++i) {
                handled = dispatchBackspaceToFocusedElement(this) || handled;
            }
        }
        m_lastSoftBackspaceTimeMs = 0;
    }

    if (handled) {
        event->accept();
        return;
    }

    QQuickItem::inputMethodEvent(event);
}

void WPEWebPage::keyPressEvent(QKeyEvent *event)
{
    if (!event) {
        return;
    }

    if (shouldInterceptSoftKeyboardEvent(event)) {
        if (event->key() == Qt::Key_Backspace) {
            dispatchBackspaceToFocusedElement(this);
            m_lastSoftBackspaceTimeMs = QDateTime::currentMSecsSinceEpoch();
        } else if (!event->text().isEmpty()) {
            dispatchTextToFocusedElement(this, event->text(), 0);
            m_lastSoftKeyboardText = event->text();
            m_lastSoftKeyboardTextTimeMs = QDateTime::currentMSecsSinceEpoch();
            m_lastPreeditText.clear();
        }
        event->accept();
        return;
    }

    WPEQtView::keyPressEvent(event);
}

void WPEWebPage::keyReleaseEvent(QKeyEvent *event)
{
    if (!event) {
        return;
    }

    if (shouldInterceptSoftKeyboardEvent(event)) {
        event->accept();
        return;
    }

    WPEQtView::keyReleaseEvent(event);
}

// On Sailfish OS, the Wayland compositor synthesises wl_pointer events from
// touch gestures at the COMPOSITOR level — completely outside Qt's control.
// These arrive as genuine QMouseEvent / QHoverEvent that bypass Qt's own
// AA_SynthesizeMouseForUnhandledTouchEvents mechanism and our event->accept()
// guard in touchEvent(). The result is that WebKit receives both a touch-scroll
// gesture AND a pointer press/motion for the same finger, activating :hover on
// whatever link is under the finger and interrupting the scroll recogniser.
//
// Guard: if any touch points are being tracked, drop all mouse/hover events.
// Real mouse/stylus input can never coexist with tracked touch on Sailfish, so
// this is safe and correct.

void WPEWebPage::mousePressEvent(QMouseEvent *event)
{
    if (!m_trackedTouchPoints.isEmpty()) {
        event->accept();
        return;
    }
    if (event) {
        m_lastInteractionX = event->localPos().x();
        m_lastInteractionY = event->localPos().y();
    }
    WPEQtView::mousePressEvent(event);
}

void WPEWebPage::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_trackedTouchPoints.isEmpty()) {
        event->accept();
        return;
    }
    if (event) {
        m_lastInteractionX = event->localPos().x();
        m_lastInteractionY = event->localPos().y();
    }
    WPEQtView::mouseReleaseEvent(event);
    scheduleVirtualKeyboardSync();
}

// Hover events are disabled permanently (setAcceptHoverEvents(false) in
// constructor). These overrides are kept as a defence-in-depth safety net
// in case some Qt code path re-enables hover acceptance unexpectedly.
void WPEWebPage::hoverEnterEvent(QHoverEvent *event)  { event->accept(); }
void WPEWebPage::hoverMoveEvent(QHoverEvent *event)   { event->accept(); }
void WPEWebPage::hoverLeaveEvent(QHoverEvent *event)  { event->accept(); }

void WPEWebPage::touchEvent(QTouchEvent *event)
{
    if (!event) {
        return;
    }

    forceActiveFocus();

    const QList<QTouchEvent::TouchPoint> activePoints = mergeTrackedTouchPoints(
        m_trackedTouchPoints,
        event->touchPoints(),
        event->type());
    const bool shouldSyncKeyboard = (event->type() == QEvent::TouchEnd && activePoints.size() <= 1);
    if (activePoints.size() >= 2) {
        const qreal pinchDistance = QLineF(activePoints.at(0).pos(), activePoints.at(1).pos()).length();
        WebKitWebView *wv = webView();
        if (wv && pinchDistance > 0.0) {
            if (!m_pinchZoomActive) {
                m_pinchZoomActive = true;
                m_pinchStartDistance = pinchDistance;
                m_pinchStartVisualScale = m_visualScale;

                QTouchEvent endEvent(QEvent::TouchEnd,
                                     event->device(),
                                     event->modifiers(),
                                     event->touchPointStates(),
                                     event->touchPoints());
                WPEQtView::touchEvent(&endEvent);
            } else if (m_pinchStartDistance > 0.0) {
                const qreal ratio = pinchDistance / m_pinchStartDistance;
                const qreal newVisualScale = std::clamp(
                    m_pinchStartVisualScale * ratio,
                    static_cast<qreal>(kMinimumPinchZoomFactor),
                    static_cast<qreal>(kMaximumPinchZoomFactor));

                if (!qFuzzyCompare(newVisualScale, m_visualScale)) {
                    m_visualScale = newVisualScale;

                    // Pinch center as percentage of item dimensions for CSS transform-origin
                    const QPointF center = (activePoints.at(0).pos() + activePoints.at(1).pos()) / 2.0;
                    const double cx_pct = (width()  > 0) ? center.x() / width()  * 100.0 : 50.0;
                    const double cy_pct = (height() > 0) ? center.y() / height() * 100.0 : 50.0;

                    // CSS transform: GPU-composited by WebKit — no layout reflow, no layout shift
                    char js[256];
                    snprintf(js, sizeof(js),
                        "document.documentElement.style.transform='scale(%.4f)';"
                        "document.documentElement.style.transformOrigin='%.1f%% %.1f%%';",
                        (double)m_visualScale, cx_pct, cy_pct);
                    webkit_web_view_evaluate_javascript(wv, js, -1, nullptr, nullptr, nullptr, nullptr, nullptr);
                }
            }
        }

        event->accept();
        return;
    }

    if (m_pinchZoomActive) {
        if (activePoints.size() < 2 || event->type() == QEvent::TouchEnd || event->type() == QEvent::TouchCancel) {
            // Commit pinch scale back into WebKit zoom and clear temporary CSS transform so
            // scrolling/hit-testing remain in native coordinates after the gesture ends.
            WebKitWebView *wv = webView();
            if (wv && !qFuzzyCompare(m_visualScale, 1.0)) {
                const double committedZoom = std::clamp(
                    currentPageZoomLevel() * static_cast<double>(m_visualScale),
                    minimumPinchZoomLevel(),
                    maximumPinchZoomLevel());
                setPageZoomLevel(committedZoom);

                webkit_web_view_evaluate_javascript(
                    wv,
                    "document.documentElement.style.transform='';"
                    "document.documentElement.style.transformOrigin='';",
                    -1, nullptr, nullptr, nullptr, nullptr, nullptr);
                m_visualScale = 1.0;
            }

            resetPinchZoomState(m_pinchZoomActive, m_pinchStartDistance, m_pinchStartZoomLevel);
            if (activePoints.isEmpty()) {
                m_trackedTouchPoints.clear();
            }
        } else {
            event->accept();
            return;
        }
    }

    if (activePoints.isEmpty() && (event->type() == QEvent::TouchEnd || event->type() == QEvent::TouchCancel)) {
        m_trackedTouchPoints.clear();
    }

    WPEQtView::touchEvent(event);

    // Accept the touch event so Qt does not synthesize QMouseEvents from it via
    // Qt::AA_SynthesizeMouseForUnhandledTouchEvents. Without this, Qt generates
    // fake mousePressEvent/mouseReleaseEvent calls from every touch, which
    // propagate to WPEQtView::mousePressEvent -> dispatchMousePressEvent ->
    // wpe_view_backend_dispatch_pointer_event(). WebKit then receives BOTH a
    // touch scroll gesture AND a pointer button press for the same gesture,
    // causing :hover activation on links and breaking the scroll recognizer.
    event->accept();

    if (shouldSyncKeyboard) {
        if (!event->touchPoints().isEmpty()) {
            const QTouchEvent::TouchPoint &point = event->touchPoints().constLast();
            m_lastInteractionX = point.pos().x();
            m_lastInteractionY = point.pos().y();
        }
        scheduleVirtualKeyboardSync();
    }
}

void WPEWebPage::scheduleVirtualKeyboardSync()
{
    QTimer::singleShot(0, this, [this]() {
        syncVirtualKeyboardToFocusedElement();
    });
    QTimer::singleShot(120, this, [this]() {
        syncVirtualKeyboardToFocusedElement();
    });
    QTimer::singleShot(320, this, [this]() {
        syncVirtualKeyboardToFocusedElement();
    });
}

void WPEWebPage::syncVirtualKeyboardToFocusedElement()
{
    WebKitWebView* wv = webView();
    if (!wv)
        return;

    const QString editableProbeScript = QStringLiteral(
        "(function(x,y){"
        "  function isEditable(e){"
        "    if(!e) return false;"
        "    if(e.isContentEditable) return true;"
        "    var tag=(e.tagName||'').toLowerCase();"
        "    if(tag==='textarea') return !e.readOnly && !e.disabled;"
        "    if(tag==='input'){"
        "      var t=(e.type||'text').toLowerCase();"
        "      var blocked={button:1,submit:1,reset:1,checkbox:1,radio:1,file:1,image:1,range:1,color:1,hidden:1};"
        "      return !blocked[t] && !e.readOnly && !e.disabled;"
        "    }"
        "    return false;"
        "  }"
        "  var active=document.activeElement;"
        "  if(isEditable(active)) return true;"
        "  if(!(x>=0 && y>=0)) return false;"
        "  var px=x, py=y;"
        "  var vv=window.visualViewport;"
        "  if(vv && vv.scale && vv.scale>0){"
        "    px=x/vv.scale; py=y/vv.scale;"
        "  }"
        "  var candidates=[document.elementFromPoint(px,py), document.elementFromPoint(x,y)];"
        "  for(var i=0;i<candidates.length;i++){"
        "    var e=candidates[i];"
        "    if(!e) continue;"
        "    if(e.closest){"
        "      var n=e.closest('input,textarea,[contenteditable=\"\"],[contenteditable=\"true\"]');"
        "      if(n) e=n;"
        "    }"
        "    if(isEditable(e)){"
        "      try{e.focus();}catch(_e){}"
        "      return true;"
        "    }"
        "  }"
        "  return false;"
        "})(%1,%2);")
        .arg(m_lastInteractionX, 0, 'f', 2)
        .arg(m_lastInteractionY, 0, 'f', 2);

    std::unique_ptr<KeyboardProbeData> data = std::make_unique<KeyboardProbeData>();
    data->page = this;
    webkit_web_view_evaluate_javascript(
        wv,
        editableProbeScript.toUtf8().constData(),
        -1,
        nullptr,
        nullptr,
        nullptr,
        onKeyboardProbeEvaluated,
        data.release());
}

void WPEWebPage::onFrameSwapped()
{
    if (!m_active)
        return;

    if (!m_painted) {
        m_painted = true;
        emit paintedChanged();
    }

    if (perfLoggingEnabled()) {
        if (!m_perfFrameLogWindow.isValid()) {
            m_perfFrameLogWindow.start();
            m_perfFramesInWindow = 0;
        }

        ++m_perfFramesInWindow;
        const qint64 elapsedMs = m_perfFrameLogWindow.elapsed();
        if (elapsedMs >= 2000) {
            const double fps = elapsedMs > 0
                    ? static_cast<double>(m_perfFramesInWindow) * 1000.0 / static_cast<double>(elapsedMs)
                    : 0.0;
            qDebug().nospace() << "[WPE-PERF] frame swaps fps=" << QString::number(fps, 'f', 1)
                               << " intervalMs=" << m_framePump.interval()
                               << " visible=" << isVisible()
                               << " active=" << m_active
                               << " throttle=" << m_throttlePainting
                               << " video=" << m_mediaVideoActive
                               << " audio=" << m_mediaAudioActive;
            m_perfFrameLogWindow.restart();
            m_perfFramesInWindow = 0;
        }
    }
    emit afterRendering();
}

// --- Find-in-page ---

static void onFindCountedMatches(WebKitFindController *, guint matchCount, gpointer userData)
{
    fprintf(stderr, "[WPE-FIND] countedMatches: %u\n", matchCount);
    auto *page = static_cast<WPEWebPage*>(userData);
    bool hasResult = matchCount > 0;
    if (hasResult != page->findInPageHasResult()) {
        page->setFindInPageHasResult(hasResult);
    }
}

static void onFindFoundText(WebKitFindController *, guint matchCount, gpointer userData)
{
    fprintf(stderr, "[WPE-FIND] foundText: %u\n", matchCount);
    auto *page = static_cast<WPEWebPage*>(userData);
    if (!page->findInPageHasResult()) {
        page->setFindInPageHasResult(true);
    }
}

static void onFindFailedToFindText(WebKitFindController *, gpointer userData)
{
    fprintf(stderr, "[WPE-FIND] failedToFindText\n");
    auto *page = static_cast<WPEWebPage*>(userData);
    if (page->findInPageHasResult()) {
        page->setFindInPageHasResult(false);
    }
}

void WPEWebPage::setFindInPageHasResult(bool has)
{
    fprintf(stderr, "[WPE-FIND] setFindInPageHasResult: %d -> %d\n", m_findInPageHasResult, has);
    if (m_findInPageHasResult != has) {
        m_findInPageHasResult = has;
        emit findInPageHasResultChanged();
    }
}

void WPEWebPage::findText(const QString &text, bool backwards)
{
    WebKitWebView *wv = webView();
    fprintf(stderr, "[WPE-FIND] findText called: text='%s' backwards=%d webView=%p\n",
            text.toUtf8().constData(), backwards, (void*)wv);
    if (!wv) return;

    WebKitFindController *fc = webkit_web_view_get_find_controller(wv);
    fprintf(stderr, "[WPE-FIND] findController=%p\n", (void*)fc);
    if (!fc) return;

    if (!m_findInitialized) {
        g_signal_connect(fc, "counted-matches", G_CALLBACK(onFindCountedMatches), this);
        g_signal_connect(fc, "found-text", G_CALLBACK(onFindFoundText), this);
        g_signal_connect(fc, "failed-to-find-text", G_CALLBACK(onFindFailedToFindText), this);
        m_findInitialized = true;
    }

    if (text.isEmpty()) {
        webkit_find_controller_search_finish(fc);
        setFindInPageHasResult(false);
        return;
    }

    guint32 opts = WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND;
    if (backwards)
        opts |= WEBKIT_FIND_OPTIONS_BACKWARDS;

    webkit_find_controller_search(fc, text.toUtf8().constData(), opts, G_MAXUINT);
    // Set result true immediately — GLib signals may not fire due to event loop issues.
    setFindInPageHasResult(true);
    // Keep toolbar visible during find
    forceChrome(true);
}

void WPEWebPage::findFinish()
{
    WebKitWebView *wv = webView();
    if (!wv) return;
    WebKitFindController *fc = webkit_web_view_get_find_controller(wv);
    if (fc)
        webkit_find_controller_search_finish(fc);
    setFindInPageHasResult(false);
    clearSelection();
}

// --- TLS / Security info ---

void WPEWebPage::updateSecurityInfo()
{
    auto *sec = static_cast<WPESecurityInfo*>(m_security);
    WebKitWebView *wv = webView();
    if (!wv) {
        fprintf(stderr, "[WPE-SEC] no webView\n");
        sec->reset();
        emit securityChanged();
        return;
    }

    GTlsCertificate *cert = nullptr;
    GTlsCertificateFlags flags = (GTlsCertificateFlags)0;
    gboolean hasTls = webkit_web_view_get_tls_info(wv, &cert, &flags);

    fprintf(stderr, "[WPE-SEC] hasTls=%d cert=%p flags=%u\n", hasTls, (void*)cert, (unsigned)flags);

    if (!hasTls || !cert) {
        sec->reset();
        emit securityChanged();
        return;
    }

    bool hasErrors = (flags != 0);
    QString subject, issuer, pem, notBefore, notAfter;
    QString issuerOrg, issuerCN, subjectOrg, errorDesc;

    GObjectClass *klass = G_OBJECT_GET_CLASS(cert);

    // Subject and issuer full DN strings (GLib 2.70+)
    gchar *subjectName = nullptr;
    gchar *issuerName = nullptr;
    if (g_object_class_find_property(klass, "subject-name")) {
        g_object_get(cert, "subject-name", &subjectName, NULL);
    }
    if (g_object_class_find_property(klass, "issuer-name")) {
        g_object_get(cert, "issuer-name", &issuerName, NULL);
    }
    if (subjectName) {
        subject = QString::fromUtf8(subjectName);
        g_free(subjectName);
    }
    if (issuerName) {
        issuer = QString::fromUtf8(issuerName);
        g_free(issuerName);
    }

    // Parse O= and CN= from DN strings
    auto parseDN = [](const QString &dn, const QString &key) -> QString {
        int idx = dn.indexOf(key + "=");
        if (idx < 0) return {};
        idx += key.length() + 1;
        int end = dn.indexOf(',', idx);
        return end > 0 ? dn.mid(idx, end - idx).trimmed() : dn.mid(idx).trimmed();
    };
    issuerOrg = parseDN(issuer, QStringLiteral("O"));
    issuerCN = parseDN(issuer, QStringLiteral("CN"));
    subjectOrg = parseDN(subject, QStringLiteral("O"));

    // Validity dates (GLib 2.70+)
    if (g_object_class_find_property(klass, "not-valid-before")) {
        GDateTime *dt = nullptr;
        g_object_get(cert, "not-valid-before", &dt, NULL);
        if (dt) {
            gchar *s = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S UTC");
            if (s) { notBefore = QString::fromUtf8(s); g_free(s); }
            g_date_time_unref(dt);
        }
    }
    if (g_object_class_find_property(klass, "not-valid-after")) {
        GDateTime *dt = nullptr;
        g_object_get(cert, "not-valid-after", &dt, NULL);
        if (dt) {
            gchar *s = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S UTC");
            if (s) { notAfter = QString::fromUtf8(s); g_free(s); }
            g_date_time_unref(dt);
        }
    }

    // PEM data
    gchar *pemData = nullptr;
    g_object_get(cert, "certificate-pem", &pemData, NULL);
    if (pemData) {
        pem = QString::fromUtf8(pemData);
        g_free(pemData);
    }

    // Fallback for subject if empty
    if (subject.isEmpty()) {
        const QUrl currentUrl(url());
        subject = currentUrl.host();
    }

    // Fallback for issuer from chain
    if (issuer.isEmpty()) {
        GTlsCertificate *issuerCert = g_tls_certificate_get_issuer(cert);
        if (issuerCert) {
            GObjectClass *ik = G_OBJECT_GET_CLASS(issuerCert);
            if (g_object_class_find_property(ik, "issuer-name")) {
                gchar *in = nullptr;
                g_object_get(issuerCert, "issuer-name", &in, NULL);
                if (in) { issuer = QString::fromUtf8(in); g_free(in); }
            }
            if (issuer.isEmpty()) issuer = QStringLiteral("Certificate Authority");
        }
    }

    // Error description
    if (hasErrors) {
        QStringList errors;
        if (flags & G_TLS_CERTIFICATE_UNKNOWN_CA) errors << QStringLiteral("Unknown CA");
        if (flags & G_TLS_CERTIFICATE_BAD_IDENTITY) errors << QStringLiteral("Bad identity");
        if (flags & G_TLS_CERTIFICATE_NOT_ACTIVATED) errors << QStringLiteral("Not yet valid");
        if (flags & G_TLS_CERTIFICATE_EXPIRED) errors << QStringLiteral("Expired");
        if (flags & G_TLS_CERTIFICATE_REVOKED) errors << QStringLiteral("Revoked");
        if (flags & G_TLS_CERTIFICATE_INSECURE) errors << QStringLiteral("Insecure algorithm");
        if (flags & G_TLS_CERTIFICATE_GENERIC_ERROR) errors << QStringLiteral("Generic error");
        errorDesc = errors.join(QStringLiteral(", "));
    }

    // WPE's public API (webkit_web_view_get_tls_info) exposes only the certificate
    // and error flags, not the negotiated cipher/protocol version — so report a
    // truthful generic label rather than fabricating a specific TLS version.
    QString cipher = hasErrors ? QStringLiteral("TLS (certificate errors)") : QStringLiteral("Encrypted (TLS)");

    fprintf(stderr, "[WPE-SEC] valid=true errors=%d subject='%s' issuer='%s' notBefore='%s' notAfter='%s'\n",
            hasErrors, subject.toUtf8().constData(), issuer.toUtf8().constData(),
            notBefore.toUtf8().constData(), notAfter.toUtf8().constData());

    sec->update(true, hasErrors, subject, issuer, cipher,
                notBefore, notAfter, pem, issuerOrg, issuerCN, subjectOrg, errorDesc);
    emit securityChanged();
}

// --- HTML <select> dropdown ---

void WPEWebPage::openSelectMenu(const QStringList &options, int selectedIndex)
{
    m_selectMenuOptions = options;
    m_selectMenuSelectedIdx = selectedIndex;
    m_selectMenuActive = true;
    emit selectMenuOptionsChanged();
    emit selectMenuSelectedIndexChanged();
    emit selectMenuActiveChanged();
}

void WPEWebPage::downloadUrl(const QString &url)
{
    WebKitWebView* wv = webView();
    if (!wv || url.isEmpty())
        return;
    webkit_web_view_download_uri(wv, url.toUtf8().constData());
}

void WPEWebPage::selectMenuOption(int index)
{
    const QString script = QStringLiteral(
        "(function(){"
        "  var el=window.__wpePendingSelect;"
        "  if(!el) return;"
        "  el.selectedIndex=%1;"
        "  el.dispatchEvent(new Event('change',{bubbles:true}));"
        "  el.dispatchEvent(new Event('input',{bubbles:true}));"
        "  window.__wpePendingSelect=null;"
        "})();"
    ).arg(index);
    runJavaScript(script);
    if (m_selectMenuActive) {
        m_selectMenuActive = false;
        emit selectMenuActiveChanged();
    }
}

void WPEWebPage::closeSelectMenu()
{
    runJavaScript(QStringLiteral("window.__wpePendingSelect=null;"));
    if (m_selectMenuActive) {
        m_selectMenuActive = false;
        emit selectMenuActiveChanged();
    }
}

void WPEWebPage::chooseFiles(const QStringList &filePaths)
{
    if (!m_fileChooserRequest) {
        fprintf(stderr, "[WPE-FILE] chooseFiles ignored (no active request)\n");
        return;
    }

    fprintf(stderr, "[WPE-FILE] chooseFiles incoming=%s\n",
            filePaths.join(QStringLiteral(" | ")).toUtf8().constData());

    QStringList normalizedPaths;
    QStringList existingPaths;
    normalizedPaths.reserve(filePaths.size());
    existingPaths.reserve(filePaths.size());
    for (const QString &entry : filePaths) {
        if (entry.isEmpty()) {
            continue;
        }
        const QUrl asUrl = QUrl::fromUserInput(entry);
        QString localPath = asUrl.isValid() && asUrl.isLocalFile() ? asUrl.toLocalFile() : entry;
        if (localPath.startsWith(QStringLiteral("home/"))) {
            localPath.prepend(QLatin1Char('/'));
        }
        localPath = QDir::cleanPath(localPath);
        if (!localPath.isEmpty()) {
            normalizedPaths.append(localPath);
            const QFileInfo fileInfo(localPath);
            if (fileInfo.exists() && fileInfo.isFile()) {
                existingPaths.append(fileInfo.absoluteFilePath());
            }
            fprintf(stderr, "[WPE-FILE] candidate localPath=%s exists=%d isFile=%d\n",
                    localPath.toUtf8().constData(), fileInfo.exists() ? 1 : 0, fileInfo.isFile() ? 1 : 0);
        }
    }

    QStringList chosenPaths = !existingPaths.isEmpty() ? existingPaths : normalizedPaths;
    if (!m_fileChooserSelectMultiple && chosenPaths.size() > 1) {
        const QString firstPath = chosenPaths.first();
        chosenPaths.clear();
        chosenPaths.append(firstPath);
    }

    if (chosenPaths.isEmpty()) {
        fprintf(stderr, "[WPE-FILE] no usable paths; cancelling chooser\n");
        clearFileChooserRequest(true);
        return;
    }

    fprintf(stderr, "[WPE-FILE] chosen=%s\n", chosenPaths.join(QStringLiteral(" | ")).toUtf8().constData());

    QVector<QByteArray> utf8Paths;
    utf8Paths.reserve(chosenPaths.size());
    QVector<const gchar*> selectedFiles;
    selectedFiles.reserve(chosenPaths.size() + 1);
    for (const QString &path : chosenPaths) {
        utf8Paths.append(path.toUtf8());
        selectedFiles.append(utf8Paths.constLast().constData());
    }
    selectedFiles.append(nullptr);

    webkit_file_chooser_request_select_files(m_fileChooserRequest, selectedFiles.constData());
    if (const gchar* const* selected = webkit_file_chooser_request_get_selected_files(m_fileChooserRequest)) {
        QStringList selectedDebug;
        for (guint i = 0; selected[i]; ++i) {
            selectedDebug.append(QString::fromUtf8(selected[i]));
        }
        fprintf(stderr, "[WPE-FILE] selected-files now=%s\n",
                selectedDebug.join(QStringLiteral(" | ")).toUtf8().constData());
    } else {
        fprintf(stderr, "[WPE-FILE] selected-files now=<none>\n");
    }
    clearFileChooserRequest(false);
}

void WPEWebPage::cancelFileChooser()
{
    fprintf(stderr, "[WPE-FILE] cancelFileChooser requested from QML\n");
    clearFileChooserRequest(true);
}

bool WPEWebPage::adBlockEnabled() const
{
    return AdBlockEngine::isEnabled();
}

void WPEWebPage::setAdBlockEnabled(bool enabled)
{
    if (AdBlockEngine::isEnabled() == enabled)
        return;
    AdBlockEngine::setEnabled(enabled);

    // The AdBlockEngine flag only gates frame-level policy and cosmetic
    // injection; the bulk of the blocking is the compiled WebKit content
    // filter installed on every tab's user content manager. Toggling used to
    // stop here, leaving those filters installed — the blocker looked
    // permanently on. Walk all live managers and install/remove accordingly
    // (takes effect on the next page load).
    const auto managers = contentBlockerManagers();
    for (WebKitUserContentManager* manager : managers) {
        if (enabled)
            ensureContentBlocker(manager);
        else
            webkit_user_content_manager_remove_all_filters(manager);
    }
    qDebug() << "[WPE-BLOCKER] ad block toggled" << (enabled ? "ON" : "OFF")
             << "across" << managers.size() << "tab(s)";

    emit adBlockEnabledChanged();
}
