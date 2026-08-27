/*
 * Atlantic Browser — background context for a WebExtension.
 *
 * Background scripts run in a plain JavaScriptCore context in the UI process
 * rather than an offscreen web view: WPE views need a real wpe_view_backend,
 * and an MV3 service worker has no DOM to lose anyway. The environment a
 * background script actually depends on — timers, console, fetch/XHR — is
 * polyfilled onto the same bridge the API shim uses (kBackgroundPreamble in
 * WebExtensionScripts.h), so this class only has to provide the four natives
 * those polyfills call.
 *
 * Consequence worth knowing: background pages declared as `background.page`
 * HTML get their <script> tags extracted and run here; the markup itself is
 * ignored, because there is no document.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <QHash>
#include <QObject>
#include <QString>

// Qt's `#define signals public` mangles a GDBus struct field of the same name;
// the GLib headers WebKit pulls in have to be seen with the keyword undefined.
#pragma push_macro("signals")
#undef signals
#include <jsc/jsc.h>
#pragma pop_macro("signals")

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class WebExtension;
class WebExtensionManager;

class WebExtensionBackground : public QObject
{
    Q_OBJECT

public:
    WebExtensionBackground(WebExtensionManager *manager, const WebExtension &extension,
                           const QString &shim, QObject *parent = nullptr);
    ~WebExtensionBackground() override;

    // Evaluates the preamble, the shim and then every background script in
    // manifest order. Returns false if the context could not be created; script
    // errors are logged and do not abort the remaining scripts.
    bool start();

    QString extensionId() const { return m_extensionId; }

    // Hands a bridge payload to __atlExtBridge.dispatch().
    void dispatch(const QString &json);

private:
    // The single native entry point (__atlNative). Timer and network calls are
    // served here; everything else goes to the manager.
    void handleNativeCall(const QString &json);
    void evaluate(const QString &code, const QString &sourceUri);
    void setTimer(int timerId, int delayMs, bool repeat);
    void clearTimer(int timerId);
    void startFetch(int requestId, const QString &url, const QString &method,
                    const QJsonObject &headers, const QString &body);
    void finishFetch(int requestId, QNetworkReply *reply);

    static void nativeCallTrampoline(const char *json, gpointer userData);

    WebExtensionManager *m_manager = nullptr;
    QString m_extensionId;
    QString m_baseDir;
    QString m_shim;
    QStringList m_scripts;
    JSCContext *m_context = nullptr;
    QHash<int, QTimer *> m_timers;
    QNetworkAccessManager *m_network = nullptr;
};
