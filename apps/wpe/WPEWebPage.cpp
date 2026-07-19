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
#include <QFile>
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
#include <array>
#include <memory>

#include "WPEQtViewLoadRequest.h"
#include "WPEUserScripts.h"

#include <wpe/webkit.h>
#include <gio/gio.h>

namespace {

constexpr double kMinimumPinchZoomFactor = 0.5;
constexpr double kMaximumPinchZoomFactor = 3.0;
constexpr int kDefaultFramePumpIntervalMs = 2000;
constexpr int kMediaInactiveDebounceMs = 400;
// Ad/tracker network blocking now lives entirely in the WebProcess adblock
// extension (atlantic-engine/web-extension), which runs the Brave/Rust engine
// against every resource request. The old per-tab WebKit content-filter path
// (WebKitUserContentFilterStore + content-blocker.json) has been removed.

// Audio output volume is handled entirely at the engine/PulseAudio level: the
// WebProcess tags its GStreamer audio streams media.role=x-maemo (via the
// WEBKIT_GST_MEDIA_ROLE env var, set in the engine runtime), so the SFOS system
// media volume (MainVolume2 / module-meego-mainvolume) attenuates browser audio
// natively through the hardware volume keys. The browser no longer reads or
// mirrors MainVolume2, and never touches el.volume — that per-element JS hack
// only worked on simple pages and double-attenuated once the engine fix landed.

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
//  - Android 14 in the platform slot: many mobile-detection sniffers only
//    match the literal "Android" or "iPhone" tokens ("Mobile" alone is not
//    enough) and serve the desktop site otherwise. We cannot claim both
//    platforms in one string, so claim Android there while the Safari-shaped
//    tail (Version/x ... Mobile Safari) keeps satisfying iPhone-style
//    sniffers — the "pretend to be everyone" chimera.
//  - SailfishOS in the platform comment slot: honest, ignored by sniffers.
//  - Atlantic/1.0 placed Chrome-style before "Mobile Safari" (the
//    "<Brand>/<ver> Mobile Safari/..." shape every UA parser handles).
// Desktop mode mirrors Safari's own "Request Desktop Website", which presents
// the macOS Safari UA — sites then serve the well-tested Mac variant instead
// of reacting to the rare "Linux + Safari" combination.
// ATLANTIC_USER_AGENT / ATLANTIC_USER_AGENT_DESKTOP override for testing
// without a rebuild.

// The OS version in the platform comment slot comes from /etc/os-release
// (VERSION_ID), trimmed to major.minor to keep the token shape stable.
static QString sailfishOsVersion()
{
    static const QString version = [] {
        QFile osRelease(QStringLiteral("/etc/os-release"));
        if (osRelease.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!osRelease.atEnd()) {
                QString line = QString::fromUtf8(osRelease.readLine()).trimmed();
                if (!line.startsWith(QStringLiteral("VERSION_ID=")))
                    continue;
                QString value = line.mid(11);
                if (value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')) && value.size() >= 2)
                    value = value.mid(1, value.size() - 2);
                const QStringList parts = value.split(QLatin1Char('.'));
                if (parts.size() >= 2)
                    return parts[0] + QLatin1Char('.') + parts[1];
                if (!value.isEmpty())
                    return value;
            }
        }
        return QStringLiteral("5.1");
    }();
    return version;
}

// iPhone-Safari UA — the exact shape verified on device to load the full
// Google Maps app; also what Cloudflare-challenged hosts get (see below).
static QString iphoneMobileUserAgent()
{
    return QStringLiteral(
        "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) "
        "AppleWebKit/605.1.15 (KHTML, like Gecko) "
        "Version/18.0 Mobile/15E148 Safari/604.1");
}

// Hosts where a Cloudflare managed challenge was observed this session.
// Cloudflare's bot heuristics reject the "pretend to be everyone" chimera UA
// (Android platform token + Safari-shaped tail — no real browser sends that
// combination), so those hosts get the plain iPhone UA instead. Populated at
// runtime by the resource-load-started watcher: the challenge interstitial is
// unmistakable — it loads /cdn-cgi/challenge-platform/ scripts from the page's
// own host. Cannot be a static list: any site may sit behind Cloudflare.
static QSet<QString> &cloudflareChallengedHosts()
{
    static QSet<QString> hosts;
    return hosts;
}

// User-selected per-site UA overrides (host → profile id), persisted in dconf
// as a JSON object and pushed process-wide from BrowserPage.qml via
// WPEWebContainer::setSiteUaOverrides (C++ cannot read dconf itself —
// MDConfItem is a no-op stub in this build).
static QHash<QString, QString> &siteUaOverridesMap()
{
    static QHash<QString, QString> overrides;
    return overrides;
}

// Predefined UA profiles selectable in Settings → "Site user agents".
// Ids are shared with SiteUaSettingsPage.qml; an unknown id means "no
// override". Version tokens are frozen-ish shapes sniffers accept, matching
// the philosophy of the built-in quirks above.
static QString uaForProfile(const QString &profileId)
{
    if (profileId == QLatin1String("chrome-android")) {
        return QStringLiteral(
            "Mozilla/5.0 (Linux; Android 14; Pixel 7) "
            "AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/130.0.0.0 Mobile Safari/537.36");
    }
    if (profileId == QLatin1String("safari-iphone"))
        return iphoneMobileUserAgent();
    if (profileId == QLatin1String("firefox-android")) {
        return QStringLiteral(
            "Mozilla/5.0 (Android 14; Mobile; rv:130.0) "
            "Gecko/130.0 Firefox/130.0");
    }
    if (profileId == QLatin1String("chrome-desktop")) {
        return QStringLiteral(
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
            "AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/130.0.0.0 Safari/537.36");
    }
    if (profileId == QLatin1String("safari-mac")) {
        return QStringLiteral(
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
            "AppleWebKit/605.1.15 (KHTML, like Gecko) "
            "Version/18.0 Safari/605.1.15");
    }
    if (profileId == QLatin1String("firefox-desktop")) {
        return QStringLiteral(
            "Mozilla/5.0 (X11; Linux x86_64; rv:130.0) "
            "Gecko/20100101 Firefox/130.0");
    }
    return QString();
}

// An entry for "example.com" covers example.com and every subdomain, so the
// user doesn't have to add www./m./maps. variants separately.
static QString siteUaOverrideForHost(const QString &hostIn)
{
    const QHash<QString, QString> &overrides = siteUaOverridesMap();
    if (overrides.isEmpty())
        return QString();
    const QString host = hostIn.toLower();
    for (auto it = overrides.constBegin(); it != overrides.constEnd(); ++it) {
        if (host == it.key() || host.endsWith(QLatin1Char('.') + it.key()))
            return uaForProfile(it.value());
    }
    return QString();
}

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
        "Mozilla/5.0 (Linux; Android 14; Mobile; SailfishOS %1) "
        "AppleWebKit/605.1.15 (KHTML, like Gecko) "
        "Version/26.0 Atlantic/1.0 Mobile Safari/605.1.15").arg(sailfishOsVersion());
}

// Google Maps lives at maps.google.<tld> and at google.<tld>/maps (the bare
// maps.google.com redirects to www.google.com/maps), so match both shapes.
static bool urlIsGoogleMaps(const QUrl &url)
{
    const QString host = url.host().toLower();
    if (host.startsWith(QStringLiteral("maps.google.")))
        return true;
    // google.<tld> or any *.google.<tld> serving the /maps app.
    if ((host == QStringLiteral("google.com") || host.endsWith(QStringLiteral(".google.com"))
         || host.contains(QStringLiteral(".google.")) || host.startsWith(QStringLiteral("google.")))
        && (url.path() == QStringLiteral("/maps") || url.path().startsWith(QStringLiteral("/maps/"))))
        return true;
    return false;
}

// Twitch lives at twitch.tv and m.twitch.tv (the bare twitch.tv redirects to the
// mobile host on a mobile UA).
static bool urlIsTwitch(const QUrl &url)
{
    const QString host = url.host().toLower();
    return host == QStringLiteral("twitch.tv")
        || host.endsWith(QStringLiteral(".twitch.tv"));
}

// Per-site UA quirk. Under our default mobile UA the Google Maps loader aborts
// with "app undefined" and leaves a blank page: its bootstrap branches on the
// platform token and has no working path for the unrecognised
// "(Linux; Mobile; SailfishOS 5.1)" combo, so it never loads the real app
// bundles. Presenting a recognised iPhone-Safari UA — the exact shape verified
// on device to load the full map — fixes it. Scoped to Maps hosts so the
// global UA (deliberately tuned for other sites) is untouched. Desktop mode
// already sends a "Macintosh" UA, which Maps recognises, so it is left alone.
//
// Twitch needs the OPPOSITE of a Safari UA. Its player chooses a playback engine
// from the UA + a codec probe: under our Safari-shaped UA it picks the Safari
// *native-HLS* path, which WPE can't actually play (it has no HLS demuxer and
// canPlayType('application/vnd.apple.mpegurl') misreports "maybe") — so every
// stream dies with "Error #4000 (not supported in this browser)". Presenting an
// Android-Chrome UA makes Twitch use its MSE + JS-transmux player instead, whose
// fragmented-MP4 H.264/AAC output WPE *does* decode (device-verified: the
// GStreamer pipeline parses video/x-h264 and plugs droidvdec for HW decode).
// Paired with the kTwitch user-script (autoplay-mute + "open in app" dismiss).
// Desktop mode's "Macintosh" UA already gets Twitch's (working) MSE player, so it
// is left alone.
QString atlanticUserAgentForUrl(const QUrl &url, bool desktopMode)
{
    // User-selected per-site override wins over every built-in quirk, and
    // (unlike the quirks) applies in desktop mode too — it is an explicit
    // choice, not a heuristic.
    const QString userOverride = siteUaOverrideForHost(url.host());
    if (!userOverride.isEmpty())
        return userOverride;
    if (!desktopMode && urlIsGoogleMaps(url))
        return iphoneMobileUserAgent();
    if (!desktopMode && cloudflareChallengedHosts().contains(url.host().toLower()))
        return iphoneMobileUserAgent();
    if (!desktopMode && urlIsTwitch(url)) {
        return QStringLiteral(
            "Mozilla/5.0 (Linux; Android 14; Pixel 7) "
            "AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/130.0.0.0 Mobile Safari/537.36");
    }
    return atlanticUserAgent(desktopMode);
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
    } else if (inputMethod->isVisible() && !data->page->subframeEditableFocused()) {
        // The probe can't see into cross-origin subframes; while one reports
        // an editable focused (editableFocus bridge), keep the keyboard up.
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

gboolean onDecidePolicy(WebKitWebView* webView, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer userData)
{
    // Before a top-level navigation commits, choose the UA for the destination
    // so per-site quirks (Google Maps, Cloudflare-challenged hosts) take effect
    // for the main document and all of its subresources. Returning FALSE lets
    // WebKit proceed with the default decision — now carrying the corrected UA.
    //
    // GOTCHA (device-proven with gdb on build 502): this callback also fires
    // for SUBFRAME navigations, and the glib API offers no way to tell them
    // apart. An iframe URL must never downgrade a quirked main-document UA —
    // the Cloudflare interstitial embeds a challenges.cloudflare.com iframe
    // and Maps embeds google.com subframes, and each one flipped the UA back
    // to the default mid-load, which is why the quirks never stuck. So only
    // apply here when the destination is itself quirked or the current main
    // document is not; the LOAD_COMMITTED handler re-syncs the UA to the real
    // main-frame URL afterwards (covers navigating AWAY from a quirked site,
    // where this heuristic keeps the quirk one request too long).
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        if (WPEWebPage* page = static_cast<WPEWebPage*>(userData)) {
            WebKitNavigationPolicyDecision* navDecision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
            WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(navDecision);
            WebKitURIRequest* request = action ? webkit_navigation_action_get_request(action) : nullptr;
            const gchar* uri = request ? webkit_uri_request_get_uri(request) : nullptr;
            if (uri && *uri) {
                const QUrl dest(QString::fromUtf8(uri));
                const gchar* mainUri = webkit_web_view_get_uri(webView);
                const QUrl mainUrl(mainUri ? QString::fromUtf8(mainUri) : QString());
                if (page->urlHasUaQuirk(dest) || !page->urlHasUaQuirk(mainUrl))
                    page->applyUserAgentForUrl(dest);
            }
        }
        return FALSE;
    }

    // Turn non-displayable responses into downloads. WebKit's default
    // response handler only auto-downloads when the server marks the response
    // as an attachment (Content-Disposition); an unsupported MIME type
    // without that header is *ignored*, leaving the tab stuck at "Loading"
    // and never starting a download.
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        WebKitResponsePolicyDecision* responseDecision = WEBKIT_RESPONSE_POLICY_DECISION(decision);
        WebKitURIResponse* response = webkit_response_policy_decision_get_response(responseDecision);
        const guint statusCode = response ? webkit_uri_response_get_status_code(response) : 0;
        if (!webkit_response_policy_decision_is_mime_type_supported(responseDecision)
            && statusCode != 204 /* No Content */) {
            webkit_policy_decision_download(decision);
            return TRUE;
        }
        return FALSE;
    }

    // Network ad/tracker blocking is handled per-request by the WebProcess
    // adblock extension (Brave/Rust engine), so this callback only routes
    // new-window navigations into the current view.
    if (type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        return FALSE;
    }

    WebKitNavigationPolicyDecision* navigationDecision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
    WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(navigationDecision);
    WebKitURIRequest* request = action ? webkit_navigation_action_get_request(action) : nullptr;
    const gchar* uri = request ? webkit_uri_request_get_uri(request) : nullptr;
    if (uri && *uri) {
        // Popup blocking. New-window navigations bypass the WebProcess adblock
        // extension (they are not subresource requests), so ad popups /
        // popunders must be filtered here before being routed into the view.
        if (AdBlockEngine::isEnabled()) {
            // A window.open with no user gesture behind it is a scripted
            // popup; nothing legitimate opens windows uninvited.
            if (action && !webkit_navigation_action_is_user_gesture(action)) {
                qInfo() << "[ADBLOCK] popup blocked (no user gesture):" << uri;
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }
            // Gesture-hijacked popups (click anywhere → ad tab) carry a real
            // gesture, so also match the destination against the filter list.
            const gchar* mainUri = webkit_web_view_get_uri(webView);
            const QUrl mainUrl(mainUri ? QString::fromUtf8(mainUri) : QString());
            if (AdBlockEngine::instance().shouldBlockPopup(mainUrl, QUrl(QString::fromUtf8(uri)))) {
                qInfo() << "[ADBLOCK] popup blocked (filter match):" << uri;
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }
        }
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

static void onInputPickerBridgeMessage(WebKitUserContentManager*, JSCValue* value, gpointer userData)
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
    page->openInputPicker(
        obj.value(QStringLiteral("type")).toString(),
        obj.value(QStringLiteral("value")).toString(),
        obj.value(QStringLiteral("min")).toString(),
        obj.value(QStringLiteral("max")).toString(),
        obj.value(QStringLiteral("step")).toString());
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

// Generic cosmetic filtering, UI-process side: kAdblockClassIdCollector posts
// the class/id names newly seen in the DOM; the engine maps them to generic
// hide rules (##.ad-banner style — a different lookup than the site-specific
// selectors installed pre-paint at load-committed). Matches are appended to a
// dedicated style element so dynamically inserted ads get hidden too.
static void onAdblockClassIdMessage(WebKitUserContentManager*, JSCValue* value, gpointer userData)
{
    WPEWebPage* page = static_cast<WPEWebPage*>(userData);
    if (!page || !value)
        return;
    if (!AdBlockEngine::isEnabled() || !AdBlockEngine::instance().isLoaded())
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

    QByteArray classes, ids;
    for (const QJsonValue& v : obj.value(QStringLiteral("c")).toArray()) {
        classes += v.toString().toUtf8();
        classes += '\n';
    }
    for (const QJsonValue& v : obj.value(QStringLiteral("i")).toArray()) {
        ids += v.toString().toUtf8();
        ids += '\n';
    }

    const QString sels = AdBlockEngine::instance().genericHides(page->url(), classes, ids);
    if (sels.isEmpty())
        return;

    // Generic selectors are plain .class/#id — valid CSS — but insert one rule
    // at a time under try/catch anyway, consistent with the specific-selector
    // sheet. Selectors are passed as a JSON array so no manual escaping.
    QJsonArray selArray;
    for (const QString& s : sels.split(QLatin1Char('\n'), QString::SkipEmptyParts))
        selArray.append(s);
    const QString js = QStringLiteral(
        "(function(){var sels=%1;"
        "var s=document.getElementById('__atl_adblock_gen_hide');"
        "if(!s){s=document.createElement('style');s.id='__atl_adblock_gen_hide';"
        "document.documentElement.appendChild(s);}"
        "var sh=s.sheet;if(!sh)return;"
        "for(var i=0;i<sels.length;i++){"
        "try{sh.insertRule(sels[i]+'{display:none!important}',sh.cssRules.length);}catch(e){}}"
        "})()").arg(QString::fromUtf8(QJsonDocument(selArray).toJson(QJsonDocument::Compact)));
    page->runJavaScript(js);
    qDebug() << "[ADBLOCK] generic hides applied:" << selArray.size() << "selectors on" << page->url().host();
}

static void onEditableFocusMessage(WebKitUserContentManager*, JSCValue* value, gpointer userData)
{
    WPEWebPage* page = static_cast<WPEWebPage*>(userData);
    if (!page || !value)
        return;

    bool focused = false;
    if (jsc_value_is_number(value))
        focused = jsc_value_to_int32(value) != 0;
    else if (jsc_value_is_boolean(value))
        focused = jsc_value_to_boolean(value);

    page->handleSubframeEditableFocus(focused);
}

// --- Cookie-banner blocking (DuckDuckGo autoconsent) ---
// The standalone autoconsent bundle self-initializes with autoAction=optOut
// and its embedded CMP rules: it detects the site's consent dialog (Didomi,
// OneTrust, Quantcast, ...) and answers "reject all" programmatically, so the
// banner closes itself AND consent is recorded as refused — unlike the
// cosmetic hide rules, which only mask the dialog. Installed per content
// manager as a document-start all-frames user script (CMPs often live in
// iframes). The hide-rule layer in engine.dat remains the fallback for dumb
// non-CMP banners autoconsent does not know.
static bool s_cookieBannerBlocking = true;
static const char* kAutoconsentScriptKey = "atlantic-autoconsent-script";

static const QByteArray& autoconsentScriptSource()
{
    static const QByteArray source = [] {
        QFile f(QString::fromLatin1(WPERuntimePaths::kAtlanticShareDir)
                + QStringLiteral("/autoconsent.js"));
        QByteArray data;
        if (f.open(QIODevice::ReadOnly))
            data = f.readAll();
        if (data.isEmpty())
            qWarning() << "[COOKIE-BANNER] autoconsent.js missing or empty at" << f.fileName();
        else
            qInfo() << "[COOKIE-BANNER] autoconsent loaded," << data.size() / 1024 << "KB";
        return data;
    }();
    return source;
}

static void installAutoconsent(WebKitUserContentManager* ucm)
{
    if (g_object_get_data(G_OBJECT(ucm), kAutoconsentScriptKey))
        return;
    const QByteArray& src = autoconsentScriptSource();
    if (src.isEmpty())
        return;
    WebKitUserScript* script = webkit_user_script_new(
        src.constData(),
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, script);
    // Keep the ref so a later toggle-off can remove exactly this script
    // (never remove_all_scripts here — the ucm carries the bridge scripts).
    g_object_set_data_full(G_OBJECT(ucm), kAutoconsentScriptKey, script,
                           reinterpret_cast<GDestroyNotify>(webkit_user_script_unref));
}

static void removeAutoconsent(WebKitUserContentManager* ucm)
{
    auto* script = static_cast<WebKitUserScript*>(
        g_object_get_data(G_OBJECT(ucm), kAutoconsentScriptKey));
    if (!script)
        return;
    webkit_user_content_manager_remove_script(ucm, script);
    g_object_set_data(G_OBJECT(ucm), kAutoconsentScriptKey, nullptr);
}

static void onSelectionBridgeInstall(WebKitUserContentManager* ucm, WPEWebPage* page)
{
    g_signal_connect(ucm, "script-message-received::selectionBridge",
                     G_CALLBACK(onSelectionBridgeMessage), page);
    webkit_user_content_manager_register_script_message_handler(ucm, "selectionBridge", nullptr);

    g_signal_connect(ucm, "script-message-received::editableFocus",
                     G_CALLBACK(onEditableFocusMessage), page);
    webkit_user_content_manager_register_script_message_handler(ucm, "editableFocus", nullptr);

    WebKitUserScript* editableFocusScript = webkit_user_script_new(
        WPEUserScripts::kEditableFocusBridge,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, editableFocusScript);
    webkit_user_script_unref(editableFocusScript);

    const gchar* selectionBridgeJs = WPEUserScripts::kSelectionBridge;

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
    const gchar* perfCssJs = WPEUserScripts::kPerfCss;
    WebKitUserScript* perfScript = webkit_user_script_new(
        perfCssJs,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
        nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, perfScript);
    webkit_user_script_unref(perfScript);

    // YouTube player icons come up blank on WPE: the legacy play button
    // (.ytp-svg-fill) computes black, and the mobile player's <c3-icon> control
    // glyphs (fullscreen/seek/mute/prev-next) fail to paint as inline <svg>.
    // kYouTubeIconFix forces the former white and re-issues the latter as
    // data-URI -webkit-mask-images sourced from YouTube's own SVG. See header.
    const gchar* ytIconFixJs = WPEUserScripts::kYouTubeIconFix;
    WebKitUserScript* ytIconFixScript = webkit_user_script_new(
        ytIconFixJs,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
        nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, ytIconFixScript);
    webkit_user_script_unref(ytIconFixScript);

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
    const gchar* iconHealJs = WPEUserScripts::kIconHeal;
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
        const gchar* passiveScrollJs = WPEUserScripts::kPassiveScroll;
        WebKitUserScript* passiveScript = webkit_user_script_new(
            passiveScrollJs,
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
            nullptr, nullptr);
        webkit_user_content_manager_add_script(ucm, passiveScript);
        webkit_user_script_unref(passiveScript);
    }

    // Reddit feed performance fix. Reddit autoplays MUTED <video> feed posts on
    // scroll-into-view (media_playback_requires_user_gesture only blocks AUDIBLE
    // autoplay), and on the Adreno 610 a continuously-decoding video competes
    // with the compositor for the GPU and holds a GStreamer pipeline + buffered
    // media resident. kRedditPerf pauses muted videos that play without a user
    // gesture (tap-to-play is preserved) and forces preload=none. Reddit-scoped
    // inside the script. Injected at DOCUMENT_START so the play-event listeners
    // are installed before Reddit's own scripts trigger autoplay. The memory/OOM
    // side is handled in the engine by webkit-memory-pressure-threshold-env.patch
    // (an earlier content-visibility approach here was removed — it halved scroll
    // fps). Set ATLANTIC_DISABLE_REDDIT_PERF=1 to disable.
    if (!envVarEnabled(qgetenv("ATLANTIC_DISABLE_REDDIT_PERF"))) {
        const gchar* redditPerfJs = WPEUserScripts::kRedditPerf;
        WebKitUserScript* redditScript = webkit_user_script_new(
            redditPerfJs,
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
            nullptr, nullptr);
        webkit_user_content_manager_add_script(ucm, redditScript);
        webkit_user_script_unref(redditScript);
    }

    // MSE prebuffer cap. hls.js buffers its SourceBuffer up to its config limits
    // independently of <video> play state, so a paused/gated feed video keeps
    // buffering the whole clip (resident media that kRedditPerf's pause() can't
    // stop). kMediaBufferCap clamps hls.js's supported maxBuffer* knobs (the
    // player just buffers less ahead; playback is unaffected). DOCUMENT_START so
    // the window.Hls hook is in place before the page's player script runs; all
    // frames since players are often iframed. Set ATLANTIC_DISABLE_MEDIA_BUFCAP=1
    // to disable.
    if (!envVarEnabled(qgetenv("ATLANTIC_DISABLE_MEDIA_BUFCAP"))) {
        const gchar* mediaBufCapJs = WPEUserScripts::kMediaBufferCap;
        WebKitUserScript* bufCapScript = webkit_user_script_new(
            mediaBufCapJs,
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
            nullptr, nullptr);
        webkit_user_content_manager_add_script(ucm, bufCapScript);
        webkit_user_script_unref(bufCapScript);
    }

    // Force HW-decodable video on YouTube. droidvdec accelerates H.264/H.265 but
    // VP8/VP9/AV1 decode in SOFTWARE, which can't keep up with YouTube's 1080p VP9
    // (device-measured ~0 fps). YouTube's player picks its codec from the
    // MediaSource/canPlayType/mediaCapabilities support probes (not the UA), and
    // always offers an avc1 format too. kYouTubeH264 makes those probes report
    // vp9/vp8/av01 as unsupported so the player falls back to avc1, which droidvdec
    // hardware-decodes (device-verified: vpx → droidvdec0:src). DOCUMENT_START, all
    // frames (the player can be iframed). YouTube-scoped inside the script. Set
    // ATLANTIC_DISABLE_YT_H264=1 to disable.
    if (!envVarEnabled(qgetenv("ATLANTIC_DISABLE_YT_H264"))) {
        const gchar* ytH264Js = WPEUserScripts::kYouTubeH264;
        WebKitUserScript* ytH264Script = webkit_user_script_new(
            ytH264Js,
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
            nullptr, nullptr);
        webkit_user_content_manager_add_script(ucm, ytH264Script);
        webkit_user_script_unref(ytH264Script);
    }

    // Twitch playback fix. The Android-Chrome UA quirk (atlanticUserAgentForUrl)
    // gets Twitch onto its MSE player so WPE can decode the stream; kTwitch then
    // clears the two gates that still stop playback — it force-mutes load-time
    // autoplay (the engine rejects audible autoplay, and Twitch tears the source
    // down on that rejection) and dismisses the "Open in App" interstitial that
    // pauses the page. DOCUMENT_START so the play() wrapper is in place before
    // Twitch's player autoplays; all frames since the player can be iframed.
    // Twitch-scoped inside the script. Set ATLANTIC_DISABLE_TWITCH=1 to disable.
    if (!envVarEnabled(qgetenv("ATLANTIC_DISABLE_TWITCH"))) {
        const gchar* twitchJs = WPEUserScripts::kTwitch;
        WebKitUserScript* twitchScript = webkit_user_script_new(
            twitchJs,
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
            nullptr, nullptr);
        webkit_user_content_manager_add_script(ucm, twitchScript);
        webkit_user_script_unref(twitchScript);
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

    Q_UNUSED(x);
    Q_UNUSED(y);
    // NB: this runs in the WebKit UCM script-message callback, which is outside
    // the QML JS execution context — emitting recvAsyncMessage here never reaches
    // the QML onRecvAsyncMessage handler ("No JavaScript engine"). Drive a NOTIFY
    // property instead, observed via a plain QML binding (like openSelectMenu).
    page->openImageLongPress(imageUrl);
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
    const gchar* scrollBridgeJs = WPEUserScripts::kScrollBridge;

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

    const gchar* mediaBridgeJs = WPEUserScripts::kMediaBridge;

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
        "  while(el && el.tagName==='IFRAME'){"
        "    try{ var d=el.contentDocument; if(!d){ el=null; break; } el=d.activeElement; }catch(_e){ el=null; break; }"
        "  }"
        "  if(!isEditable(el)) return false;"
        "  var doc=el.ownerDocument||document;"
        "  var win=doc.defaultView||window;"
        "  if(el.isContentEditable){"
        "    var sel=win.getSelection();"
        "    if(!sel || !sel.rangeCount) return false;"
        "    if(doc.queryCommandSupported && doc.queryCommandSupported('insertText')){"
        "      if(replaceBefore>0){"
        "        for(var i=0;i<replaceBefore;i++) doc.execCommand('delete', false, null);"
        "      }"
        "      if(t.length) doc.execCommand('insertText', false, t);"
        "      return true;"
        "    }"
        "    var r=sel.getRangeAt(0);"
        "    if(r.collapsed && replaceBefore>0){"
        "      var available = Math.min(replaceBefore, r.startOffset);"
        "      if(available>0) r.setStart(r.startContainer, r.startOffset-available);"
        "    }"
        "    r.deleteContents();"
        "    if(t.length){"
        "      var node=doc.createTextNode(t);"
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
        "  while(el && el.tagName==='IFRAME'){"
        "    try{ var d=el.contentDocument; if(!d){ el=null; break; } el=d.activeElement; }catch(_e){ el=null; break; }"
        "  }"
        "  if(!isEditable(el)) return false;"
        "  var doc=el.ownerDocument||document;"
        "  var win=doc.defaultView||window;"
        "  if(el.isContentEditable){"
        "    var sel=win.getSelection();"
        "    if(!sel || !sel.rangeCount) return false;"
        "    if(doc.queryCommandSupported && doc.queryCommandSupported('delete')){"
        "      doc.execCommand('delete', false, null);"
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

static QList<WPEWebPage *> &liveWebPages()
{
    static QList<WPEWebPage *> s_pages;
    return s_pages;
}

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

    liveWebPages().append(this);

    // Pages start inactive (background); only activatePage() flips them
    // visible. Without this, a restored-but-never-activated tab would keep
    // the plugin's default visible state and burn CPU in the background.
    setWebKitVisible(false);

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
            g_signal_connect(wv, "decide-policy", G_CALLBACK(onDecidePolicy), this);
            // Cloudflare managed-challenge detection: the interstitial loads
            // its scripts from /cdn-cgi/challenge-platform/ on the page's own
            // host (unlike an embedded Turnstile widget, which loads from
            // challenges.cloudflare.com — deliberately NOT matched, or any
            // page with a captcha widget would reload out from under the
            // user). First sighting on a host: remember it, switch to the
            // iPhone UA and reload so Cloudflare sees a UA it accepts.
            // Cross-checking navigator.userAgent against the header is part
            // of the challenge, so a header-only rewrite would not pass.
            g_signal_connect(
                wv, "resource-load-started",
                G_CALLBACK(+[](WebKitWebView* view, WebKitWebResource*, WebKitURIRequest* request,
                               gpointer userData) {
                    auto *page = static_cast<WPEWebPage*>(userData);
                    const gchar* uri = request ? webkit_uri_request_get_uri(request) : nullptr;
                    if (!page || page->desktopMode() || !uri)
                        return;
                    const QUrl resourceUrl(QString::fromUtf8(uri));
                    if (!resourceUrl.path().startsWith(QStringLiteral("/cdn-cgi/challenge-platform/")))
                        return;
                    const QString pageHost = QUrl(QString::fromUtf8(webkit_web_view_get_uri(view))).host().toLower();
                    if (pageHost.isEmpty() || resourceUrl.host().toLower() != pageHost
                        || cloudflareChallengedHosts().contains(pageHost))
                        return;
                    cloudflareChallengedHosts().insert(pageHost);
                    qInfo("[Atlantic] Cloudflare challenge on %s — retrying with iPhone UA", qPrintable(pageHost));
                    page->applyUserAgentForUrl(page->url());
                    webkit_web_view_reload(view);
                }),
                this);
            // Authoritative UA re-sync: load-changed only fires for the main
            // frame, so at commit webkit_web_view_get_uri IS the main document
            // URL. Corrects the one case the decide-policy heuristic above
            // gets wrong (leaving a quirked site keeps the quirk UA for that
            // first request) so it never lingers past the commit.
            g_signal_connect(
                wv, "load-changed",
                G_CALLBACK(+[](WebKitWebView* view, WebKitLoadEvent event, gpointer userData) {
                    if (event != WEBKIT_LOAD_COMMITTED)
                        return;
                    const gchar* uri = webkit_web_view_get_uri(view);
                    if (uri && *uri)
                        static_cast<WPEWebPage*>(userData)->applyUserAgentForUrl(QUrl(QString::fromUtf8(uri)));
                }),
                this);
            // Geolocation and camera/microphone permissions: prompt the user
            // via a QML banner (the qt5 plugin only auto-allows device-info).
            g_signal_connect(
                wv, "permission-request",
                G_CALLBACK(+[](WebKitWebView*, WebKitPermissionRequest* request, gpointer userData) -> gboolean {
                    if (!WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(request)
                        && !WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(request))
                        return FALSE;
                    static_cast<WPEWebPage*>(userData)->handlePermissionRequest(request);
                    return TRUE;
                }),
                this);
            // TLS certificate failures (self-signed, expired, ...): WebKit blocks the
            // load. Surface the failure to QML so the user can accept the certificate
            // for this host and retry. Return FALSE so "load-failed" still fires and
            // the normal failed-load path resets the loading state.
            g_signal_connect(
                wv, "load-failed-with-tls-errors",
                G_CALLBACK(+[](WebKitWebView*, const gchar* failingURI, GTlsCertificate* cert,
                               GTlsCertificateFlags flags, gpointer userData) -> gboolean {
                    auto *page = static_cast<WPEWebPage*>(userData);
                    if (page)
                        page->handleTlsErrorLoadFailed(QString::fromUtf8(failingURI), cert, (unsigned)flags);
                    return FALSE;
                }),
                this);
            {
                static bool engineInitialized = false;
                if (!engineInitialized) {
                    engineInitialized = true;
                    // The persisted toggle is pushed by BrowserPage.qml's
                    // ConfigurationValue (Component.onCompleted fires during
                    // QML load, well before the first web view exists) via
                    // webView.setAdBlockEnabled → applyAdBlockEnabledGlobally.
                    // Do NOT read it here with MDConfItem: the in-tree
                    // MDConfItem is a no-op stub (apps/core/mdconfitem.h,
                    // real mlite5 SIGSEGVs in this app), so value() always
                    // returns the default and would stomp the QML-pushed
                    // state back to enabled on every start.
                    qInfo() << "[ADBLOCK] enabled (persisted):" << AdBlockEngine::isEnabled();
                    QString cachePath = QStringLiteral("/usr/share/atlantic-browser/engine.dat");
                    if (!QFileInfo::exists(cachePath)) {
                        cachePath = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
                                    + QStringLiteral("/atlantic-browser/engine.dat");
                    }
                    if (!AdBlockEngine::instance().loadFromCache(cachePath)) {
                        qWarning() << "[ADBLOCK] engine not available — falling back to content blocker only";
                    } else {
                        // Scriptlet resources live next to the engine cache;
                        // without them every ##+js(...) rule is a no-op.
                        AdBlockEngine::instance().loadResources(
                            QFileInfo(cachePath).path() + QStringLiteral("/adblock-resources.json"));
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

            // Pre-paint cosmetic ad hiding: at load-committed the URL is final
            // but nothing has been parsed or painted yet, so a user style
            // sheet installed here hides ads before their first layout —
            // no render-then-remove flash (the old path injected the CSS via
            // runJavaScript 300ms AFTER LoadSucceeded, i.e. two layout shifts
            // per server-rendered ad, e.g. reddit's shreddit-ad-post).
            g_signal_connect(
                wv, "load-changed",
                G_CALLBACK(+[](WebKitWebView* view, WebKitLoadEvent event, gpointer) {
                    if (event != WEBKIT_LOAD_COMMITTED)
                        return;
                    const gchar* uri = webkit_web_view_get_uri(view);
                    if (!uri)
                        return;
                    AdBlockEngine::instance().installCosmetics(
                        webkit_web_view_get_user_content_manager(view),
                        QUrl(QString::fromUtf8(uri)));
                }),
                nullptr);

            // Generic cosmetic rules need the class/id names present in the
            // DOM: install the collector (document-start, batched) and its
            // message handler. Cheap when adblock is off (handler no-ops).
            g_signal_connect(ucm, "script-message-received::adblockClassId",
                             G_CALLBACK(onAdblockClassIdMessage), this);
            webkit_user_content_manager_register_script_message_handler(ucm, "adblockClassId", nullptr);
            {
                WebKitUserScript* collector = webkit_user_script_new(
                    WPEUserScripts::kAdblockClassIdCollector,
                    WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                    WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                    nullptr, nullptr);
                webkit_user_content_manager_add_script(ucm, collector);
                webkit_user_script_unref(collector);
            }

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
            qDebug("[WPE-SELECT] register_script_message_handler returned %d, ucm=%p", (int)regOk, ucm);

            // Inject JS: intercept <select> taps via multiple event types
            const gchar* selectBridgeJs = WPEUserScripts::kSelectBridge;
            WebKitUserScript* script = webkit_user_script_new(
                selectBridgeJs,
                WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                nullptr, nullptr);
            webkit_user_content_manager_add_script(ucm, script);
            webkit_user_script_unref(script);

            // --- HTML5 date/time/color input pickers via JS bridge ---
            g_signal_connect(ucm, "script-message-received::inputPickerBridge",
                             G_CALLBACK(onInputPickerBridgeMessage), this);
            webkit_user_content_manager_register_script_message_handler(ucm, "inputPickerBridge", nullptr);
            WebKitUserScript* inputPickerScript = webkit_user_script_new(
                WPEUserScripts::kInputPickerBridge,
                WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                nullptr, nullptr);
            webkit_user_content_manager_add_script(ucm, inputPickerScript);
            webkit_user_script_unref(inputPickerScript);

            onSelectionBridgeInstall(ucm, this);
            if (s_cookieBannerBlocking)
                installAutoconsent(ucm);
            onImageLongPressBridgeInstall(ucm, this);
            onScrollBridgeInstall(ucm, this);
            onMediaBridgeInstall(ucm, this);

            WebKitNetworkSession* session = webkit_web_view_get_network_session(wv);
            if (!session) {
                session = webkit_network_session_get_default();
            }
            // Private tabs run on an ephemeral session: never bind it to on-disk
            // cookie storage — that would defeat the point of private browsing
            // (and persistent storage is invalid on an ephemeral session).
            WebKitCookieManager* cookieManager =
                (session && !privateBrowsing())
                    ? webkit_network_session_get_cookie_manager(session) : nullptr;
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

const QList<WPEWebPage *> &WPEWebPage::liveInstances()
{
    return liveWebPages();
}

WPEWebPage::~WPEWebPage()
{
    if (m_pendingPermission) {
        webkit_permission_request_deny(WEBKIT_PERMISSION_REQUEST(m_pendingPermission));
        g_object_unref(m_pendingPermission);
        m_pendingPermission = nullptr;
    }
    liveWebPages().removeAll(this);
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

    // Output volume is owned by the user via the system media volume (hardware
    // keys), applied natively in PulseAudio because the WebProcess tags its audio
    // streams media.role=x-maemo. The browser does not read, mirror, or write the
    // system volume, and does not touch el.volume. We only track the page-reported
    // level/mute for reference; nothing here attenuates playback.
    if (volume < 0.0)
        volume = 0.0;
    if (volume > 1.0)
        volume = 1.0;
    m_mediaVolume = volume;
    m_mediaMuted = muted;
    Q_UNUSED(volumeChangedByPage);
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
        updateWebKitVisibility();
        emit activeChanged();
    }
}

void WPEWebPage::setAppForeground(bool foreground)
{
    if (m_appForeground != foreground) {
        m_appForeground = foreground;
        updateWebKitVisibility();
    }
}

void WPEWebPage::updateWebKitVisibility()
{
    // Background tabs and a minimized app must report a hidden page to
    // WebKit, otherwise rAF loops and timer storms (e.g. Divi-built sites)
    // keep a WebProcess core pinned indefinitely.
    const bool visible = m_active && m_appForeground;
    if (visible != webKitVisible()) {
        qDebug() << "[WPE-VIS] tabId=" << m_tabId << "webkit visible ->" << visible
                 << "(active=" << m_active << "appForeground=" << m_appForeground << ")";
        setWebKitVisible(visible);
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
    // Route through the per-URL picker so a desktop-mode toggle while on a
    // quirked site (e.g. Google Maps) keeps the correct UA.
    applyUserAgentForUrl(url());
    if (changed) {
        emit desktopModeChanged();
        // Reload so the server sends the correct page variant.
        setUrl(url());
    }
}

void WPEWebPage::applyUserAgentForUrl(const QUrl &url)
{
    setUserAgent(atlanticUserAgentForUrl(url, m_desktopMode));
}

bool WPEWebPage::urlHasUaQuirk(const QUrl &url) const
{
    return atlanticUserAgentForUrl(url, m_desktopMode) != atlanticUserAgent(m_desktopMode);
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
        // Emit a proper data: URI so consumers (FavoriteIcon.qml et al.) can render
        // it directly. Without the prefix the raw base64 was fed to image://theme/,
        // producing broken favicon tiles and log spam.
        emit ctx->page->thumbnailResult(QStringLiteral("data:image/png;base64,")
                                        + QString::fromLatin1(ba.toBase64()));
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

void WPEWebPage::handleSubframeEditableFocus(bool focused)
{
    m_subframeEditableFocus = focused;

    if (!focused)
        return;

    // Only pop the keyboard for the visible tab — a background tab's iframe
    // grabbing focus must not yank the keyboard from under the user.
    if (!isVisible())
        return;

    if (QInputMethod* inputMethod = QGuiApplication::inputMethod())
        inputMethod->show();
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
        // A top-level navigation defocuses whatever field the user was typing
        // in — dismiss the virtual keyboard like other mobile browsers do
        // (e.g. after submitting a search). Only for the visible tab, so a
        // background tab load doesn't yank the keyboard mid-typing.
        if (isVisible()) {
            if (QInputMethod* inputMethod = QGuiApplication::inputMethod()) {
                if (inputMethod->isVisible())
                    inputMethod->hide();
            }
        }
        clearFileChooserRequest(true);
        m_subframeEditableFocus = false;
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
        // Dismiss a pending TLS-error banner on new navigation
        if (m_tlsErrorPending) {
            m_tlsErrorPending = false;
            m_tlsErrorFailingUri.clear();
            m_tlsErrorHost.clear();
            m_tlsErrorMessage.clear();
            if (m_tlsErrorCert) {
                g_object_unref(m_tlsErrorCert);
                m_tlsErrorCert = nullptr;
            }
            emit tlsErrorChanged();
        }
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
    qDebug("[WPE-FILE] open request=%p selectMultiple=%d mimeTypes=%s",
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
    qDebug("[WPE-FILE] clear request=%p cancel=%d active=%d",
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
    const bool committedIsNewlineOnly = !committed.isEmpty()
        && (committed == QLatin1String("\n") || committed == QLatin1String("\r") || committed == QLatin1String("\r\n"));
    if (committedIsNewlineOnly) {
        // Maliit can deliver the Enter key as an IM commit of a bare newline
        // (instead of, or in addition to, a Key_Return event). Route it through
        // the Enter dispatcher, skipping duplicates of a just-handled key event.
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const bool duplicateOfRecentEnter =
            (nowMs - m_lastSoftEnterTimeMs >= 0) && (nowMs - m_lastSoftEnterTimeMs < 200);
        if (!duplicateOfRecentEnter) {
            sendNativeEnterKey();
            handled = true;
            m_lastSoftEnterTimeMs = nowMs;
        }
        m_lastSoftKeyboardText.clear();
        m_lastSoftKeyboardTextTimeMs = 0;
        m_lastPreeditText.clear();
    } else if (!committed.isEmpty()) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const bool duplicateOfRecentSoftKey =
            (committed == m_lastSoftKeyboardText) &&
            (nowMs - m_lastSoftKeyboardTextTimeMs >= 0) &&
            (nowMs - m_lastSoftKeyboardTextTimeMs < 200);

        if (!duplicateOfRecentSoftKey) {
            const int replaceBefore = m_lastPreeditText.size();
            if (m_subframeEditableFocus) {
                // JS dispatch can't reach a cross-origin subframe; type natively.
                sendNativeTextViaKeys(committed, replaceBefore);
                handled = true;
            } else {
                handled = dispatchTextToFocusedElement(this, committed, replaceBefore) || handled;
            }
        }

        m_lastSoftKeyboardText.clear();
        m_lastSoftKeyboardTextTimeMs = 0;
        m_lastPreeditText.clear();
    } else if (!preedit.isEmpty()) {
        if (m_subframeEditableFocus) {
            sendNativeTextViaKeys(preedit, m_lastPreeditText.size());
            handled = true;
        } else {
            handled = dispatchTextToFocusedElement(this, preedit, m_lastPreeditText.size()) || handled;
        }
        m_lastPreeditText = preedit;
    } else if (!m_lastPreeditText.isEmpty()) {
        if (m_subframeEditableFocus) {
            sendNativeTextViaKeys(QString(), m_lastPreeditText.size());
            handled = true;
        } else {
            handled = dispatchTextToFocusedElement(this, QString(), m_lastPreeditText.size()) || handled;
        }
        m_lastPreeditText.clear();
    }

    const int deleteCount = event->replacementLength();
    if (deleteCount > 0) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const bool duplicateOfRecentSoftBackspace =
            (nowMs - m_lastSoftBackspaceTimeMs >= 0) &&
            (nowMs - m_lastSoftBackspaceTimeMs < 200);
        if (!duplicateOfRecentSoftBackspace) {
            if (m_subframeEditableFocus) {
                sendNativeTextViaKeys(QString(), deleteCount);
                handled = true;
            } else {
                for (int i = 0; i < deleteCount; ++i) {
                    handled = dispatchBackspaceToFocusedElement(this) || handled;
                }
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

// Synthesize a native Enter press/release through the WPE key path. Used when
// Maliit delivers Enter as an IM commit of a bare newline (no QKeyEvent): only
// the native path produces trusted key events and WebKit's real default action
// (implicit form submission / newline).
void WPEWebPage::sendNativeEnterKey()
{
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    WPEQtView::keyPressEvent(&press);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    WPEQtView::keyReleaseEvent(&release);
}

// Type text through the native WPE key path, one code point per press/release
// (preceded by backspaces to retract a pending preedit). This is the only way
// to reach an input focused inside a CROSS-ORIGIN iframe: the JS dispatch runs
// in the main frame and cannot touch the subframe's document, while native key
// events are routed by WebKit to the focused frame regardless of origin.
void WPEWebPage::sendNativeTextViaKeys(const QString& text, int replaceBefore)
{
    for (int i = 0; i < replaceBefore; ++i) {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
        WPEQtView::keyPressEvent(&press);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Backspace, Qt::NoModifier);
        WPEQtView::keyReleaseEvent(&release);
    }
    for (int i = 0; i < text.size(); ++i) {
        const bool pair = text.at(i).isHighSurrogate() && i + 1 < text.size();
        const QString ch = text.mid(i, pair ? 2 : 1);
        if (pair)
            ++i;
        QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, ch);
        WPEQtView::keyPressEvent(&press);
        QKeyEvent release(QEvent::KeyRelease, 0, Qt::NoModifier, ch);
        WPEQtView::keyReleaseEvent(&release);
    }
}

void WPEWebPage::keyPressEvent(QKeyEvent *event)
{
    if (!event) {
        return;
    }

    if (shouldInterceptSoftKeyboardEvent(event)) {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            // The VKB Enter arrives with text "\n"/"\r", which the generic soft-key
            // path would insert as a literal character — a no-op in single-line
            // inputs, and sites ignore synthetic (untrusted) JS key events. Forward
            // it through the native WPE key path instead so WebKit fires trusted
            // keydown/keypress and performs the real default action (form submit /
            // search, newline in textareas).
            m_lastSoftEnterTimeMs = QDateTime::currentMSecsSinceEpoch();
            m_lastPreeditText.clear();
            WPEQtView::keyPressEvent(event);
            event->accept();
            return;
        }
        if (m_subframeEditableFocus) {
            // Cross-origin subframe focused: the JS dispatch can't reach it,
            // but the native key path can — forward the event unmodified.
            if (event->key() == Qt::Key_Backspace) {
                m_lastSoftBackspaceTimeMs = QDateTime::currentMSecsSinceEpoch();
            } else if (!event->text().isEmpty()) {
                m_lastSoftKeyboardText = event->text();
                m_lastSoftKeyboardTextTimeMs = QDateTime::currentMSecsSinceEpoch();
                m_lastPreeditText.clear();
            }
            WPEQtView::keyPressEvent(event);
            event->accept();
            return;
        }
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
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter
            || m_subframeEditableFocus)
            WPEQtView::keyReleaseEvent(event); // complete the native press/release pair
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

    if (event->type() == QEvent::TouchBegin) {
        emit touched();
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
        "  function deepActive(doc){"
        "    var a=doc.activeElement;"
        "    while(a && a.tagName==='IFRAME'){"
        "      try{ var d=a.contentDocument; if(!d) break; a=d.activeElement; }catch(_e){ break; }"
        "    }"
        "    return a;"
        "  }"
        "  function hitTest(doc,hx,hy,depth){"
        "    var e=doc.elementFromPoint(hx,hy);"
        "    if(!e) return null;"
        "    if(e.tagName==='IFRAME' && depth<4){"
        "      try{"
        "        var d=e.contentDocument;"
        "        if(d){"
        "          var r=e.getBoundingClientRect();"
        "          var inner=hitTest(d,hx-r.left,hy-r.top,depth+1);"
        "          if(inner) return inner;"
        "        }"
        "      }catch(_e){}"
        "    }"
        "    if(e.closest){"
        "      var n=e.closest('input,textarea,[contenteditable=\"\"],[contenteditable=\"true\"]');"
        "      if(n) e=n;"
        "    }"
        "    return e;"
        "  }"
        "  var active=deepActive(document);"
        "  if(isEditable(active)) return true;"
        "  if(!(x>=0 && y>=0)) return false;"
        "  var px=x, py=y;"
        "  var vv=window.visualViewport;"
        "  if(vv && vv.scale && vv.scale>0){"
        "    px=x/vv.scale; py=y/vv.scale;"
        "  }"
        "  var candidates=[hitTest(document,px,py,0), hitTest(document,x,y,0)];"
        "  for(var i=0;i<candidates.length;i++){"
        "    var e=candidates[i];"
        "    if(!e) continue;"
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
    qDebug("[WPE-FIND] countedMatches: %u", matchCount);
    auto *page = static_cast<WPEWebPage*>(userData);
    bool hasResult = matchCount > 0;
    if (hasResult != page->findInPageHasResult()) {
        page->setFindInPageHasResult(hasResult);
    }
}

static void onFindFoundText(WebKitFindController *, guint matchCount, gpointer userData)
{
    qDebug("[WPE-FIND] foundText: %u", matchCount);
    auto *page = static_cast<WPEWebPage*>(userData);
    if (!page->findInPageHasResult()) {
        page->setFindInPageHasResult(true);
    }
}

static void onFindFailedToFindText(WebKitFindController *, gpointer userData)
{
    qDebug("[WPE-FIND] failedToFindText");
    auto *page = static_cast<WPEWebPage*>(userData);
    if (page->findInPageHasResult()) {
        page->setFindInPageHasResult(false);
    }
}

void WPEWebPage::setFindInPageHasResult(bool has)
{
    qDebug("[WPE-FIND] setFindInPageHasResult: %d -> %d", m_findInPageHasResult, has);
    if (m_findInPageHasResult != has) {
        m_findInPageHasResult = has;
        emit findInPageHasResultChanged();
    }
}

void WPEWebPage::findText(const QString &text, bool backwards)
{
    WebKitWebView *wv = webView();
    qDebug("[WPE-FIND] findText called: text='%s' backwards=%d webView=%p",
           text.toUtf8().constData(), backwards, (void*)wv);
    if (!wv) return;

    WebKitFindController *fc = webkit_web_view_get_find_controller(wv);
    qDebug("[WPE-FIND] findController=%p", (void*)fc);
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

void WPEWebPage::handleTlsErrorLoadFailed(const QString &failingUri, void *certificate, unsigned flags)
{
    if (m_tlsErrorCert)
        g_object_unref(m_tlsErrorCert);
    m_tlsErrorCert = certificate ? g_object_ref(certificate) : nullptr;
    m_tlsErrorFailingUri = failingUri;
    m_tlsErrorHost = QUrl(failingUri).host();

    QStringList errors;
    if (flags & G_TLS_CERTIFICATE_UNKNOWN_CA) errors << QStringLiteral("the certificate is not signed by a trusted authority (self-signed?)");
    if (flags & G_TLS_CERTIFICATE_BAD_IDENTITY) errors << QStringLiteral("the certificate does not match this site");
    if (flags & G_TLS_CERTIFICATE_NOT_ACTIVATED) errors << QStringLiteral("the certificate is not yet valid");
    if (flags & G_TLS_CERTIFICATE_EXPIRED) errors << QStringLiteral("the certificate has expired");
    if (flags & G_TLS_CERTIFICATE_REVOKED) errors << QStringLiteral("the certificate has been revoked");
    if (flags & G_TLS_CERTIFICATE_INSECURE) errors << QStringLiteral("the certificate uses an insecure algorithm");
    if (flags & G_TLS_CERTIFICATE_GENERIC_ERROR) errors << QStringLiteral("the certificate could not be validated");
    m_tlsErrorMessage = errors.join(QStringLiteral("; "));

    m_tlsErrorPending = true;
    qWarning("[WPE-SEC] TLS error for host '%s': %s",
             m_tlsErrorHost.toUtf8().constData(), m_tlsErrorMessage.toUtf8().constData());
    emit tlsErrorChanged();
}

void WPEWebPage::acceptTlsCertificate()
{
    WebKitWebView *wv = webView();
    if (!wv || !m_tlsErrorPending || !m_tlsErrorCert || m_tlsErrorHost.isEmpty())
        return;
    if (WebKitNetworkSession *session = webkit_web_view_get_network_session(wv)) {
        webkit_network_session_allow_tls_certificate_for_host(
            session, G_TLS_CERTIFICATE(m_tlsErrorCert), m_tlsErrorHost.toUtf8().constData());
        qWarning("[WPE-SEC] user accepted certificate for host '%s' — retrying %s",
                 m_tlsErrorHost.toUtf8().constData(), m_tlsErrorFailingUri.toUtf8().constData());
        // NOT webkit_web_view_reload(): the TLS failure aborted the provisional
        // load, so there is no committed page to reload — reload() is a no-op
        // (device-proven: 34 accepts, 0 retries). Re-request the failing URI.
        webkit_web_view_load_uri(wv, m_tlsErrorFailingUri.toUtf8().constData());
    }
    // Pending state (and the cert ref) is cleared by the LoadStarted reset.
}

static QString permissionRequestType(WebKitPermissionRequest *req)
{
    if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(req))
        return QStringLiteral("geolocation");
    auto *media = WEBKIT_USER_MEDIA_PERMISSION_REQUEST(req);
    const bool video = webkit_user_media_permission_is_for_video_device(media);
    const bool audio = webkit_user_media_permission_is_for_audio_device(media);
    if (video && audio)
        return QStringLiteral("camera+microphone");
    return video ? QStringLiteral("camera") : QStringLiteral("microphone");
}

void WPEWebPage::handlePermissionRequest(void *request)
{
    auto *req = WEBKIT_PERMISSION_REQUEST(request);
    const QString host = url().host();
    const QString type = permissionRequestType(req);

    // Session-remembered decision for this host+type: resolve silently.
    auto it = m_permissionDecisions.constFind(host + QLatin1Char('|') + type);
    if (it != m_permissionDecisions.constEnd()) {
        if (it.value())
            webkit_permission_request_allow(req);
        else
            webkit_permission_request_deny(req);
        return;
    }

    // A second request while one is pending (e.g. another frame): deny the
    // newcomer rather than dropping the one the user is looking at.
    if (m_pendingPermission) {
        webkit_permission_request_deny(req);
        return;
    }

    m_pendingPermission = g_object_ref(request);
    m_permissionHost = host;
    m_permissionType = type;
    qWarning("[WPE-PERM] %s permission requested by '%s'",
             type.toUtf8().constData(), host.toUtf8().constData());
    emit permissionChanged();
}

void WPEWebPage::resolvePermission(bool allow)
{
    if (!m_pendingPermission)
        return;
    auto *req = WEBKIT_PERMISSION_REQUEST(m_pendingPermission);
    if (allow)
        webkit_permission_request_allow(req);
    else
        webkit_permission_request_deny(req);
    m_permissionDecisions.insert(m_permissionHost + QLatin1Char('|') + m_permissionType, allow);
    qWarning("[WPE-PERM] %s %s for '%s'", m_permissionType.toUtf8().constData(),
             allow ? "allowed" : "denied", m_permissionHost.toUtf8().constData());
    g_object_unref(m_pendingPermission);
    m_pendingPermission = nullptr;
    m_permissionHost.clear();
    m_permissionType.clear();
    emit permissionChanged();
}

void WPEWebPage::updateSecurityInfo()
{
    auto *sec = static_cast<WPESecurityInfo*>(m_security);
    WebKitWebView *wv = webView();
    if (!wv) {
        qWarning("[WPE-SEC] no webView");
        sec->reset();
        emit securityChanged();
        return;
    }

    GTlsCertificate *cert = nullptr;
    GTlsCertificateFlags flags = (GTlsCertificateFlags)0;
    gboolean hasTls = webkit_web_view_get_tls_info(wv, &cert, &flags);

    qDebug("[WPE-SEC] hasTls=%d cert=%p flags=%u", hasTls, (void*)cert, (unsigned)flags);

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

    qDebug("[WPE-SEC] valid=true errors=%d subject='%s' issuer='%s' notBefore='%s' notAfter='%s'",
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

void WPEWebPage::openInputPicker(const QString &type, const QString &value,
                                 const QString &min, const QString &max, const QString &step)
{
    m_inputPickerType = type;
    m_inputPickerValue = value;
    m_inputPickerMin = min;
    m_inputPickerMax = max;
    m_inputPickerStep = step;
    m_inputPickerActive = true;
    emit inputPickerChanged();
    emit inputPickerActiveChanged();
}

void WPEWebPage::resolveInputPicker(const QString &value)
{
    // Assign to the pending input and fire input+change so page scripts and form
    // state observe the new value, then clear the pending reference. The value is
    // injected as the sole element of a JSON array literal ((["…"])[0]) so any
    // quotes/backslashes survive without hand-rolled escaping.
    const QByteArray enc = QJsonDocument(QJsonArray{ value }).toJson(QJsonDocument::Compact);
    const QString script = QStringLiteral(
        "(function(){"
        "  var el=window.__wpePendingInput;"
        "  if(!el) return;"
        "  el.value=(%1)[0];"
        "  el.dispatchEvent(new Event('input',{bubbles:true}));"
        "  el.dispatchEvent(new Event('change',{bubbles:true}));"
        "  window.__wpePendingInput=null;"
        "})();"
    ).arg(QString::fromUtf8(enc));
    runJavaScript(script);
    if (m_inputPickerActive) {
        m_inputPickerActive = false;
        emit inputPickerActiveChanged();
    }
}

void WPEWebPage::cancelInputPicker()
{
    runJavaScript(QStringLiteral("window.__wpePendingInput=null;"));
    if (m_inputPickerActive) {
        m_inputPickerActive = false;
        emit inputPickerActiveChanged();
    }
}

void WPEWebPage::openImageLongPress(const QString &imageUrl)
{
    if (m_imageLongPressUrl != imageUrl) {
        m_imageLongPressUrl = imageUrl;
        emit imageLongPressUrlChanged();
    }
}

void WPEWebPage::clearImageLongPress()
{
    if (!m_imageLongPressUrl.isEmpty()) {
        m_imageLongPressUrl.clear();
        emit imageLongPressUrlChanged();
    }
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
        qWarning("[WPE-FILE] chooseFiles ignored (no active request)");
        return;
    }

    qDebug("[WPE-FILE] chooseFiles incoming=%s",
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
            qDebug("[WPE-FILE] candidate localPath=%s exists=%d isFile=%d",
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
        qWarning("[WPE-FILE] no usable paths; cancelling chooser");
        clearFileChooserRequest(true);
        return;
    }

    qDebug("[WPE-FILE] chosen=%s", chosenPaths.join(QStringLiteral(" | ")).toUtf8().constData());

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
        qDebug("[WPE-FILE] selected-files now=%s",
               selectedDebug.join(QStringLiteral(" | ")).toUtf8().constData());
    } else {
        qDebug("[WPE-FILE] selected-files now=<none>");
    }
    clearFileChooserRequest(false);
}

void WPEWebPage::cancelFileChooser()
{
    qDebug("[WPE-FILE] cancelFileChooser requested from QML");
    clearFileChooserRequest(true);
}

bool WPEWebPage::adBlockEnabled() const
{
    return AdBlockEngine::isEnabled();
}

void WPEWebPage::setAdBlockEnabled(bool enabled)
{
    applyAdBlockEnabledGlobally(enabled);
}

bool WPEWebPage::cookieBannerBlockingEnabled() const
{
    return s_cookieBannerBlocking;
}

void WPEWebPage::setCookieBannerBlockingEnabled(bool enabled)
{
    applyCookieBannerBlockingGlobally(enabled);
}

void WPEWebPage::applyCookieBannerBlockingGlobally(bool enabled)
{
    if (s_cookieBannerBlocking == enabled)
        return;
    s_cookieBannerBlocking = enabled;

    // Add/remove the autoconsent user script on every live content manager.
    // User scripts only run at document start, so the change takes effect on
    // the next (re)load of each tab — a banner already answered stays answered.
    for (WPEWebPage* page : liveInstances()) {
        if (WebKitWebView* wv = page->webView()) {
            WebKitUserContentManager* ucm = webkit_web_view_get_user_content_manager(wv);
            if (enabled)
                installAutoconsent(ucm);
            else
                removeAutoconsent(ucm);
        }
        emit page->cookieBannerBlockingEnabledChanged();
    }
    qInfo() << "[COOKIE-BANNER] blocking toggled" << (enabled ? "ON" : "OFF");
}

void WPEWebPage::applySiteUaOverridesGlobally(const QString &json)
{
    QHash<QString, QString> parsed;
    const QJsonObject obj = QJsonDocument::fromJson(json.toUtf8()).object();
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        const QString host = it.key().toLower().trimmed();
        const QString profile = it.value().toString();
        if (!host.isEmpty() && !profile.isEmpty())
            parsed.insert(host, profile);
    }
    if (siteUaOverridesMap() == parsed)
        return;

    // Snapshot each tab's effective UA before swapping the map, so only tabs
    // the edit actually affects get reloaded below.
    QHash<WPEWebPage *, QString> uaBefore;
    for (WPEWebPage *page : liveInstances())
        uaBefore.insert(page, atlanticUserAgentForUrl(page->url(), page->m_desktopMode));

    siteUaOverridesMap() = parsed;

    // Re-resolve every live tab's UA, and reload the tabs whose UA changed —
    // an override edit is an explicit user action on a site that (typically)
    // misbehaves under the current UA, so waiting for a manual reload would
    // just look like the setting did nothing.
    for (WPEWebPage *page : liveInstances()) {
        page->applyUserAgentForUrl(page->url());
        const QString uaNow = atlanticUserAgentForUrl(page->url(), page->m_desktopMode);
        if (uaNow != uaBefore.value(page) && !page->url().isEmpty()) {
            if (WebKitWebView *view = page->webView())
                webkit_web_view_reload(view);
        }
    }
    qInfo() << "[Atlantic] site UA overrides updated:" << parsed.size() << "entries";
}

void WPEWebPage::applyAdBlockEnabledGlobally(bool enabled)
{
    if (AdBlockEngine::isEnabled() == enabled)
        return;
    AdBlockEngine::setEnabled(enabled);

    // Network blocking lives in the WebProcess adblock extension; tell every
    // live page so the change takes effect in all tabs — and all WebProcesses —
    // without a reload. New WebProcesses pick up the state via the extension's
    // init user-data. The AdBlockEngine flag still gates cosmetic injection in
    // the UI process.
    for (WPEWebPage *page : liveInstances()) {
        if (WebKitWebView *wv = page->webView()) {
            webkit_web_view_send_message_to_page(
                wv, webkit_user_message_new("atlantic-adblock-set-enabled",
                                            g_variant_new_boolean(enabled)),
                nullptr, nullptr, nullptr);
            // Cosmetic sheets: tear down on disable so hidden ads reappear
            // without a reload; on enable, reinstall for the current page
            // (future navigations install at load-committed as usual).
            WebKitUserContentManager* ucm = webkit_web_view_get_user_content_manager(wv);
            if (!enabled) {
                AdBlockEngine::resetCosmetics(ucm);
                // Also drop the generic-hide rules already injected in the page.
                page->runJavaScript(QStringLiteral(
                    "(function(){var s=document.getElementById('__atl_adblock_gen_hide');"
                    "if(s)s.parentNode.removeChild(s);})()"));
            } else if (const gchar* uri = webkit_web_view_get_uri(wv)) {
                AdBlockEngine::instance().installCosmetics(ucm, QUrl(QString::fromUtf8(uri)));
            }
        }
        emit page->adBlockEnabledChanged();
    }
    qDebug() << "[WPE-BLOCKER] ad block toggled" << (enabled ? "ON" : "OFF");
}

void WPEWebPage::applyAdBlockAllowlistGlobally(const QString &json)
{
    // dconf JSON array of hosts on which the ad blocker is disabled.
    QStringList hosts;
    const QJsonArray arr = QJsonDocument::fromJson(json.toUtf8()).array();
    for (const QJsonValue &v : arr) {
        const QString host = v.toString().toLower().trimmed();
        if (!host.isEmpty())
            hosts.append(host);
    }

    // Snapshot which live pages the edit changes, before swapping the list.
    QHash<WPEWebPage *, bool> allowedBefore;
    for (WPEWebPage *page : liveInstances())
        allowedBefore.insert(page, AdBlockEngine::isAllowlistedUrl(page->url()));

    AdBlockEngine::setAllowlist(hosts);

    // Tell every WebProcess (network blocking); new WebProcesses get the list
    // via the extension's init user-data.
    const QByteArray joined = AdBlockEngine::allowlistJoined();
    for (WPEWebPage *page : liveInstances()) {
        WebKitWebView *wv = page->webView();
        if (!wv)
            continue;
        webkit_web_view_send_message_to_page(
            wv, webkit_user_message_new("atlantic-adblock-set-allowlist",
                                        g_variant_new_string(joined.constData())),
            nullptr, nullptr, nullptr);

        // Pages whose allowlist state changed: tear down the per-view cosmetic
        // sheets (they are keyed by host and survive reloads) and reload — the
        // toggle is an explicit user action on this site, a stale half-blocked
        // page would look like it did nothing. Load-committed reinstalls
        // cosmetics where still appropriate.
        if (allowedBefore.value(page) != AdBlockEngine::isAllowlistedUrl(page->url())
            && !page->url().isEmpty()) {
            AdBlockEngine::resetCosmetics(webkit_web_view_get_user_content_manager(wv));
            webkit_web_view_reload(wv);
        }
    }
    qInfo() << "[ADBLOCK] allowlist updated:" << hosts.size() << "hosts";
}
