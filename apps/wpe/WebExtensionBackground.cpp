/*
 * Atlantic Browser — background context for a WebExtension.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "WebExtensionBackground.h"

#include "WebExtension.h"
#include "WebExtensionManager.h"
#include "WebExtensionScripts.h"

#include <QBuffer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

namespace {

// JSON-encodes a value into a literal that can be pasted into evaluated JS.
// Wrapping in a one-element array and stripping the brackets is the only way
// QJsonDocument will serialize a bare string or number for us.
QString jsonLiteral(const QJsonValue &value)
{
    QByteArray json = QJsonDocument(QJsonArray{ value }).toJson(QJsonDocument::Compact).trimmed();
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

// A JS string literal for `text`, safe to paste into evaluated source.
QString jsQuote(const QString &text)
{
    return jsonLiteral(QJsonValue(text));
}

} // namespace

WebExtensionBackground::WebExtensionBackground(WebExtensionManager *manager,
                                               const WebExtension &extension,
                                               const QString &shim, QObject *parent)
    : QObject(parent)
    , m_manager(manager)
    , m_extensionId(extension.id())
    , m_baseDir(extension.baseDir())
    , m_shim(shim)
    , m_scripts(extension.backgroundScripts())
{
    // A background *page* is HTML we cannot render here; pull its <script src>
    // list out so at least the code runs. Inline <script> bodies are ignored —
    // extensions that need those will not work, and say so in the log.
    const QString page = extension.backgroundPage();
    if (!page.isEmpty()) {
        QFile file(QDir(m_baseDir).filePath(page));
        if (file.open(QIODevice::ReadOnly)) {
            const QString html = QString::fromUtf8(file.readAll());
            QRegularExpression tag(QStringLiteral("<script[^>]*\\ssrc\\s*=\\s*[\"']([^\"']+)[\"']"),
                                   QRegularExpression::CaseInsensitiveOption);
            auto it = tag.globalMatch(html);
            while (it.hasNext())
                m_scripts << it.next().captured(1);
            if (html.contains(QRegularExpression(QStringLiteral("<script(?![^>]*\\ssrc)"),
                                                 QRegularExpression::CaseInsensitiveOption))) {
                qWarning() << "[WEBEXT]" << m_extensionId
                           << "background page has inline <script>, which is not executed";
            }
        }
    }
    m_scripts.removeDuplicates();
}

WebExtensionBackground::~WebExtensionBackground()
{
    qDeleteAll(m_timers);
    m_timers.clear();
    if (m_context)
        g_object_unref(m_context);
}

void WebExtensionBackground::nativeCallTrampoline(const char *json, gpointer userData)
{
    auto *self = static_cast<WebExtensionBackground *>(userData);
    if (self && json)
        self->handleNativeCall(QString::fromUtf8(json));
}

bool WebExtensionBackground::start()
{
    if (m_scripts.isEmpty())
        return false;

    m_context = jsc_context_new();
    if (!m_context) {
        qWarning() << "[WEBEXT]" << m_extensionId << "could not create a JS context";
        return false;
    }

    JSCValue *native = jsc_value_new_function(
        m_context, "__atlNative", G_CALLBACK(nativeCallTrampoline), this, nullptr,
        G_TYPE_NONE, 1, G_TYPE_STRING);
    jsc_context_set_value(m_context, "__atlNative", native);
    g_object_unref(native);

    evaluate(QString::fromUtf8(WebExtensionScripts::kBackgroundPreamble),
             QStringLiteral("atlantic-extension://%1/_preamble.js").arg(m_extensionId));
    evaluate(m_shim, QStringLiteral("atlantic-extension://%1/_shim.js").arg(m_extensionId));

    const QStringList scripts = m_scripts;
    for (const QString &relative : scripts) {
        const QString path = QDir(m_baseDir).filePath(relative);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "[WEBEXT]" << m_extensionId << "missing background script" << relative;
            continue;
        }
        evaluate(QString::fromUtf8(file.readAll()),
                 QStringLiteral("atlantic-extension://%1/%2").arg(m_extensionId, relative));
    }

    // Extensions commonly do their one-time setup from onInstalled/onStartup.
    // We have no install/update distinction yet, so every launch is a startup.
    dispatch(QString::fromUtf8(QJsonDocument(QJsonObject{
        { QStringLiteral("type"), QStringLiteral("event") },
        { QStringLiteral("name"), QStringLiteral("runtime.onStartup") },
        { QStringLiteral("args"), QJsonArray() }
    }).toJson(QJsonDocument::Compact)));

    return true;
}

void WebExtensionBackground::evaluate(const QString &code, const QString &sourceUri)
{
    if (!m_context || code.isEmpty())
        return;
    const QByteArray utf8 = code.toUtf8();
    const QByteArray uri = sourceUri.toUtf8();
    JSCValue *result = jsc_context_evaluate_with_source_uri(
        m_context, utf8.constData(), utf8.size(), uri.constData(), 1);
    if (result)
        g_object_unref(result);

    if (JSCException *exception = jsc_context_get_exception(m_context)) {
        qWarning("[WEBEXT] %s: %s (%s:%u)", qPrintable(m_extensionId),
                 jsc_exception_get_message(exception), uri.constData(),
                 jsc_exception_get_line_number(exception));
        jsc_context_clear_exception(m_context);
    }
}

void WebExtensionBackground::dispatch(const QString &json)
{
    if (!m_context)
        return;
    evaluate(QStringLiteral("__atlExtBridge && __atlExtBridge.dispatch(%1);").arg(jsQuote(json)),
             QStringLiteral("atlantic-extension://%1/_dispatch.js").arg(m_extensionId));
}

void WebExtensionBackground::handleNativeCall(const QString &json)
{
    const QJsonObject payload = QJsonDocument::fromJson(json.toUtf8()).object();
    const QString api = payload.value(QStringLiteral("api")).toString();
    const QJsonArray args = payload.value(QStringLiteral("args")).toArray();

    // Timers and network are ours; everything else is a browser.* API call.
    if (api == QLatin1String("timer.set")) {
        setTimer(args.at(0).toInt(), args.at(1).toInt(), args.at(2).toBool());
        return;
    }
    if (api == QLatin1String("timer.clear")) {
        clearTimer(args.at(0).toInt());
        return;
    }
    if (api == QLatin1String("net.fetch")) {
        startFetch(args.at(0).toInt(), args.at(1).toString(), args.at(2).toString(),
                   args.at(3).toObject(), args.at(4).isNull() ? QString() : args.at(4).toString());
        return;
    }

    if (m_manager)
        m_manager->handleBridgeMessage(m_extensionId, nullptr, false, json);
}

void WebExtensionBackground::setTimer(int timerId, int delayMs, bool repeat)
{
    clearTimer(timerId);
    auto *timer = new QTimer(this);
    timer->setSingleShot(!repeat);
    timer->setInterval(qMax(0, delayMs));
    connect(timer, &QTimer::timeout, this, [this, timerId, repeat]() {
        evaluate(QStringLiteral("__atlFireTimer(%1);").arg(timerId),
                 QStringLiteral("atlantic-extension://%1/_timer.js").arg(m_extensionId));
        if (!repeat)
            clearTimer(timerId);
    });
    m_timers.insert(timerId, timer);
    timer->start();
}

void WebExtensionBackground::clearTimer(int timerId)
{
    if (QTimer *timer = m_timers.take(timerId)) {
        timer->stop();
        timer->deleteLater();
    }
}

void WebExtensionBackground::startFetch(int requestId, const QString &url, const QString &method,
                                        const QJsonObject &headers, const QString &body)
{
    const QUrl target(url);
    if (!target.isValid() || (target.scheme() != QLatin1String("http")
                              && target.scheme() != QLatin1String("https"))) {
        // Extension-local reads go through the same fetch() call in real
        // browsers; serve them straight off disk instead of over the network.
        if (target.scheme() == QLatin1String(WebExtensionManager::kScheme)) {
            QFile file(QDir(m_baseDir).filePath(target.path().mid(1)));
            const bool ok = file.open(QIODevice::ReadOnly);
            const QString content = ok ? QString::fromUtf8(file.readAll()) : QString();
            evaluate(QStringLiteral("__atlFetchDone(%1,%2,%3,\"\",{},%4,%5);")
                         .arg(requestId)
                         .arg(ok ? QStringLiteral("true") : QStringLiteral("false"))
                         .arg(ok ? 200 : 404)
                         .arg(jsQuote(content),
                              ok ? QStringLiteral("null") : jsQuote(QStringLiteral("not found"))),
                     QStringLiteral("atlantic-extension://%1/_fetch.js").arg(m_extensionId));
            return;
        }
        evaluate(QStringLiteral("__atlFetchDone(%1,false,0,\"\",{},null,%2);")
                     .arg(requestId)
                     .arg(jsQuote(QStringLiteral("unsupported URL: %1").arg(url))),
                 QStringLiteral("atlantic-extension://%1/_fetch.js").arg(m_extensionId));
        return;
    }

    if (!m_network)
        m_network = new QNetworkAccessManager(this);

    QNetworkRequest request(target);
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        request.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());

    const QByteArray verb = method.toUpper().toUtf8();
    const QByteArray payload = body.toUtf8();
    QNetworkReply *reply = nullptr;
    if (verb == "GET") {
        reply = m_network->get(request);
    } else if (verb == "HEAD") {
        reply = m_network->head(request);
    } else if (verb == "POST") {
        reply = m_network->post(request, payload);
    } else if (verb == "PUT") {
        reply = m_network->put(request, payload);
    } else if (verb == "DELETE" && payload.isEmpty()) {
        reply = m_network->deleteResource(request);
    } else {
        // Qt 5.6 has no QByteArray overload of sendCustomRequest; the buffer
        // has to outlive the call, so it is parented onto the reply below.
        auto *buffer = new QBuffer;
        buffer->setData(payload);
        buffer->open(QIODevice::ReadOnly);
        reply = m_network->sendCustomRequest(request, verb, buffer);
        buffer->setParent(reply);
    }

    connect(reply, &QNetworkReply::finished, this, [this, requestId, reply]() {
        finishFetch(requestId, reply);
        reply->deleteLater();
    });
}

void WebExtensionBackground::finishFetch(int requestId, QNetworkReply *reply)
{
    const bool ok = reply->error() == QNetworkReply::NoError;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();

    QJsonObject headers;
    const QList<QNetworkReply::RawHeaderPair> pairs = reply->rawHeaderPairs();
    for (const QNetworkReply::RawHeaderPair &pair : pairs)
        headers.insert(QString::fromUtf8(pair.first), QString::fromUtf8(pair.second));

    const QString body = QString::fromUtf8(reply->readAll());
    evaluate(QStringLiteral("__atlFetchDone(%1,%2,%3,%4,%5,%6,%7);")
                 .arg(requestId)
                 .arg(ok ? QStringLiteral("true") : QStringLiteral("false"))
                 .arg(status)
                 .arg(jsQuote(reason),
                      jsonLiteral(headers),
                      jsQuote(body),
                      ok ? QStringLiteral("null") : jsQuote(reply->errorString())),
             QStringLiteral("atlantic-extension://%1/_fetch.js").arg(m_extensionId));
}
