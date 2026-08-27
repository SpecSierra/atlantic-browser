/*
 * Atlantic Browser — background page for a WebExtension, in a real web view.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "WebExtensionBackgroundView.h"

#include "WebExtension.h"
#include "WebExtensionManager.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>

namespace {

// The offscreen view still runs the full compositor path; it just has nowhere
// to put the result. Acknowledge every frame at once and release the buffer, or
// WebKit stops painting after the first one and the page's rendering-driven
// timers (rAF, and anything gated on layout) stall.
using ExportableHolder = WebExtensionBackgroundView::ExportableHolder;

const struct wpe_view_backend_exportable_fdo_client kDiscardingClient = {
    // export_buffer_resource
    [](void *data, struct wl_resource *buffer) {
        auto *holder = static_cast<ExportableHolder *>(data);
        if (!holder->exportable)
            return;
        wpe_view_backend_exportable_fdo_dispatch_release_buffer(holder->exportable, buffer);
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(holder->exportable);
    },
    // export_dmabuf_resource
    [](void *data, struct wpe_view_backend_exportable_fdo_dmabuf_resource *resource) {
        auto *holder = static_cast<ExportableHolder *>(data);
        if (!holder->exportable)
            return;
        if (resource && resource->buffer_resource)
            wpe_view_backend_exportable_fdo_dispatch_release_buffer(holder->exportable,
                                                                    resource->buffer_resource);
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(holder->exportable);
    },
    // export_shm_buffer
    [](void *data, struct wpe_fdo_shm_exported_buffer *buffer) {
        auto *holder = static_cast<ExportableHolder *>(data);
        if (!holder->exportable)
            return;
        wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(holder->exportable,
                                                                             buffer);
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(holder->exportable);
    },
    nullptr, nullptr
};

QString sanitize(const QString &id)
{
    QString out = id;
    for (int i = 0; i < out.size(); ++i) {
        const QChar c = out.at(i);
        if (!c.isLetterOrNumber() && c != QLatin1Char('_'))
            out[i] = QLatin1Char('_');
    }
    return out;
}

QString jsQuote(const QString &text)
{
    QByteArray json = QJsonDocument(QJsonArray{ text }).toJson(QJsonDocument::Compact).trimmed();
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

} // namespace

WebExtensionBackgroundView::WebExtensionBackgroundView(WebExtensionManager *manager,
                                                       const WebExtension &extension,
                                                       const QString &shim, QObject *parent)
    : QObject(parent)
    , m_manager(manager)
    , m_extensionId(extension.id())
    , m_shim(shim)
{
    // A declared page is loaded as-is. Otherwise the scheme handler synthesises
    // one listing background.scripts, the same trick Chrome and Firefox use.
    const QString page = extension.backgroundPage();
    m_backgroundUrl = QStringLiteral("%1://%2/%3")
                          .arg(QLatin1String(WebExtensionManager::kScheme), m_extensionId,
                               page.isEmpty()
                                   ? QStringLiteral("_generated_background_page.html")
                                   : page);
}

WebExtensionBackgroundView::~WebExtensionBackgroundView()
{
    if (m_webView) {
        // try_close() only *asks* the page to close, and a background page has
        // no UI to honour the request — the view outlived us and kept its whole
        // WebProcess alive, so a reload() left one orphan per extension behind.
        // Terminate the process, then drop our reference.
        webkit_web_view_terminate_web_process(m_webView);
        g_object_unref(m_webView);
        m_webView = nullptr;
    }
    if (m_exportable) {
        wpe_view_backend_exportable_fdo_destroy(m_exportable);
        m_exportable = nullptr;
    }
    delete m_holder;
    m_holder = nullptr;
}

QString WebExtensionBackgroundView::handlerName() const
{
    return QStringLiteral("atlExtBg_") + sanitize(m_extensionId);
}

void WebExtensionBackgroundView::onScriptMessage(WebKitUserContentManager *, JSCValue *value,
                                                 gpointer userData)
{
    auto *self = static_cast<WebExtensionBackgroundView *>(userData);
    if (!self || !self->m_manager || !value)
        return;
    if (gchar *json = jsc_value_to_string(value)) {
        // page = nullptr marks the background context, exactly as the JSC host
        // does, so the manager's routing does not need to know the difference.
        self->m_manager->handleBridgeMessage(self->m_extensionId, nullptr, false,
                                             QString::fromUtf8(json));
        g_free(json);
    }
}

bool WebExtensionBackgroundView::start()
{
    // WPEBackend-fdo needs initialising before any backend is made. The Qt view
    // does this for EGL when the first tab is created, which may not have
    // happened yet — and for the non-EGL path there is nothing to bind anyway.
    m_holder = new ExportableHolder;
    m_exportable = wpe_view_backend_exportable_fdo_create(&kDiscardingClient, m_holder, 1, 1);
    if (!m_exportable) {
        delete m_holder;
        m_holder = nullptr;
        qWarning() << "[WEBEXT]" << m_extensionId
                   << "could not create an offscreen backend for its background page";
        return false;
    }
    m_holder->exportable = m_exportable;

    struct wpe_view_backend *backend =
        wpe_view_backend_exportable_fdo_get_view_backend(m_exportable);
    if (!backend) {
        wpe_view_backend_exportable_fdo_destroy(m_exportable);
        m_exportable = nullptr;
        return false;
    }

    // Without this the page counts as hidden and WebKit throttles its timers -
    // which for a background page means it does nothing at all.
    wpe_view_backend_add_activity_state(backend, wpe_view_activity_state_visible
                                                 | wpe_view_activity_state_focused
                                                 | wpe_view_activity_state_in_window);

    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    const QByteArray handler = handlerName().toUtf8();
    const QByteArray detail = (QStringLiteral("script-message-received::") + handlerName()).toUtf8();
    g_signal_connect(ucm, detail.constData(), G_CALLBACK(onScriptMessage), this);
    webkit_user_content_manager_register_script_message_handler(ucm, handler.constData(), nullptr);

    // The shim runs in the page's own world, before anything else on the page.
    const QByteArray shim = m_shim.toUtf8();
    WebKitUserScript *script = webkit_user_script_new(
        shim.constData(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);

    WebKitWebViewBackend *viewBackend = webkit_web_view_backend_new(
        backend,
        +[](gpointer data) {
            // Ownership note: the exportable owns the wpe_view_backend, and is
            // destroyed by us, not here.
            (void)data;
        },
        this);

    m_webView = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
                                             "backend", viewBackend,
                                             "web-context", webkit_web_context_get_default(),
                                             "user-content-manager", ucm,
                                             nullptr));
    g_object_unref(ucm);
    if (!m_webView) {
        qWarning() << "[WEBEXT]" << m_extensionId << "could not create its background view";
        return false;
    }

    if (WebKitSettings *settings = webkit_web_view_get_settings(m_webView)) {
        // The background page's console is the only window onto an extension's
        // own diagnostics, so route it to the browser log.
        webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);
        webkit_settings_set_enable_developer_extras(settings, TRUE);
        webkit_settings_set_javascript_can_access_clipboard(settings, FALSE);
        // Nothing is displayed, so anything that only affects presentation is
        // wasted work in a process we want to stay cheap.
        webkit_settings_set_auto_load_images(settings, FALSE);
        webkit_settings_set_enable_smooth_scrolling(settings, FALSE);
    }

    g_signal_connect(m_webView, "load-failed",
                     G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent, gchar *uri, GError *error,
                                    gpointer data) -> gboolean {
                         auto *self = static_cast<WebExtensionBackgroundView *>(data);
                         qWarning("[WEBEXT] %s: background page %s failed to load: %s",
                                  qPrintable(self->m_extensionId), uri,
                                  error ? error->message : "unknown error");
                         return FALSE;
                     }),
                     this);

    webkit_web_view_load_uri(m_webView, m_backgroundUrl.toUtf8().constData());
    qDebug() << "[WEBEXT]" << m_extensionId << "background page:" << m_backgroundUrl;
    return true;
}

void WebExtensionBackgroundView::dispatch(const QString &json)
{
    if (!m_webView)
        return;
    const QByteArray script =
        QStringLiteral("if (typeof __atlExtBridge !== 'undefined') __atlExtBridge.dispatch(%1);")
            .arg(jsQuote(json))
            .toUtf8();
    webkit_web_view_evaluate_javascript(m_webView, script.constData(), -1, nullptr, nullptr,
                                        nullptr, nullptr, nullptr);
}
