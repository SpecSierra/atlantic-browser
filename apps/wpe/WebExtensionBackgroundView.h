/*
 * Atlantic Browser — background page for a WebExtension, in a real web view.
 *
 * The JSC host (WebExtensionBackground) runs background scripts in a bare
 * JavaScriptCore context. That is enough for service-worker-shaped extensions,
 * and it is not enough for the many Firefox extensions whose background is an
 * MV3 *event page* — a real document with DOM, localStorage, Blob, Worker,
 * XMLHttpRequest and IndexedDB. LanguageTool is one: its background reaches for
 * `document` 52 times, and under the JSC host its check pipeline simply stalls
 * with no error to show for it.
 *
 * So the background gets a genuine WebKitWebView that is never displayed. The
 * view is built on a non-EGL exportable WPEBackend-fdo backend whose exported
 * frames are acknowledged and dropped immediately: WebKit believes it has a
 * compositor and lays out and paints as usual, and the pixels go nowhere. That
 * costs a WebProcess per background page, which is the price of running an
 * extension the way it was written.
 *
 * The page runs the same API shim as any extension page, posting over
 * window.webkit.messageHandlers, so the manager's routing is unchanged: this is
 * still the extension's background context.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <QObject>
#include <QString>

// Qt's `signals` keyword collides with a GDBus struct field of the same name.
#pragma push_macro("signals")
#undef signals
#include <wpe/webkit.h>
// fdo.h refuses to be included alongside wpe/webkit-web-extension.h, and its
// sub-headers refuse to be included directly; it already pulls in the
// exportable backend API.
#include <wpe/fdo.h>
#pragma pop_macro("signals")

class WebExtension;
class WebExtensionManager;

class WebExtensionBackgroundView : public QObject
{
    Q_OBJECT

public:
    WebExtensionBackgroundView(WebExtensionManager *manager, const WebExtension &extension,
                               const QString &shim, QObject *parent = nullptr);
    ~WebExtensionBackgroundView() override;

    // Creates the offscreen view and loads the extension's background page.
    // Returns false if the backend could not be created, which is the caller's
    // cue to fall back to the JSC host rather than leaving the extension dead.
    bool start();

    QString extensionId() const { return m_extensionId; }
    // Handler name this page posts bridge messages on.
    QString handlerName() const;

    // Hands a bridge payload to __atlExtBridge.dispatch() in the page.
    void dispatch(const QString &json);

public:
    // Public only so the backend's C frame callbacks can name it.
    struct ExportableHolder {
        struct wpe_view_backend_exportable_fdo *exportable = nullptr;
    };

private:
    static void onScriptMessage(WebKitUserContentManager *ucm, JSCValue *value, gpointer userData);

    WebExtensionManager *m_manager = nullptr;
    QString m_extensionId;
    QString m_backgroundUrl;
    QString m_shim;
    // Holds the exportable so the backend's frame callbacks can reach it: they
    // are set up before it exists, so they cannot capture it directly.
    ExportableHolder *m_holder = nullptr;
    struct wpe_view_backend_exportable_fdo *m_exportable = nullptr;
    WebKitWebView *m_webView = nullptr;
};
