/*
 * Atlantic Browser — browser.downloads for the WebExtension host.
 *
 * Backed by DownloadManager, which drives every transfer the browser makes.
 * It grew a per-download record for this: the transfer engine owns the UI and
 * the progress, but it cannot answer search(), and neither could the old
 * bookkeeping, which was keyed by transfer id and thrown away on completion.
 *
 * Records live for the session only — the download/transfer mappings are not
 * persistent either — so search() answers about this run of the browser. That
 * is a real limit and is documented rather than papered over.
 *
 * What is not here, and why: pause/resume (WebKit's download API has no pause),
 * open/show/showDefaultFolder (nothing to hand a file to on this platform yet)
 * and getFileIcon. Each rejects with a message instead of pretending.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "WebExtensionManager.h"

#include "WPEWebPage.h"
#include "downloadmanager.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>

#include <algorithm>

namespace {

QString isoTime(const QDateTime &time)
{
    return time.isValid() ? time.toUTC().toString(Qt::ISODate) : QString();
}

QJsonObject recordToJson(const DownloadManager::Record &record)
{
    return QJsonObject{
        { QStringLiteral("id"), record.id },
        { QStringLiteral("url"), record.url },
        { QStringLiteral("finalUrl"), record.url },
        { QStringLiteral("referrer"), QString() },
        { QStringLiteral("filename"), record.path },
        { QStringLiteral("mime"), record.mimeType },
        { QStringLiteral("state"), record.state },
        { QStringLiteral("error"), record.error },
        { QStringLiteral("bytesReceived"), double(record.bytesReceived) },
        { QStringLiteral("totalBytes"), double(record.totalBytes) },
        { QStringLiteral("fileSize"),
          record.state == QLatin1String("complete") ? double(record.bytesReceived) : -1.0 },
        { QStringLiteral("startTime"), isoTime(record.startTime) },
        { QStringLiteral("endTime"), isoTime(record.endTime) },
        { QStringLiteral("exists"),
          record.state == QLatin1String("complete") && !record.path.isEmpty()
              && QFileInfo::exists(record.path) },
        { QStringLiteral("incognito"), false },
        // No pause support in WebKit's download API, so this is never true.
        { QStringLiteral("paused"), false },
        { QStringLiteral("canResume"), false },
        // Every download here was asked for, one way or another.
        { QStringLiteral("danger"), QStringLiteral("safe") },
        { QStringLiteral("byExtensionId"), QString() }
    };
}

bool matchesQuery(const DownloadManager::Record &record, const QJsonObject &query)
{
    const auto given = [&query](const char *key) {
        return query.contains(QLatin1String(key)) && !query.value(QLatin1String(key)).isNull();
    };

    if (given("id") && query.value(QStringLiteral("id")).toInt() != record.id)
        return false;
    if (given("url") && query.value(QStringLiteral("url")).toString() != record.url)
        return false;
    if (given("state") && query.value(QStringLiteral("state")).toString() != record.state)
        return false;
    if (given("error") && query.value(QStringLiteral("error")).toString() != record.error)
        return false;
    if (given("paused") && query.value(QStringLiteral("paused")).toBool())
        return false; // nothing is ever paused
    if (given("filename")
        && query.value(QStringLiteral("filename")).toString() != record.path) {
        return false;
    }
    if (given("exists")) {
        const bool exists = !record.path.isEmpty() && QFileInfo::exists(record.path);
        if (query.value(QStringLiteral("exists")).toBool() != exists)
            return false;
    }

    // `query` is a list of substrings that must all appear in the url or the
    // filename, as the API defines it.
    if (query.value(QStringLiteral("query")).isArray()) {
        const QJsonArray terms = query.value(QStringLiteral("query")).toArray();
        for (const QJsonValue &value : terms) {
            const QString term = value.toString();
            if (term.isEmpty())
                continue;
            const bool negated = term.startsWith(QLatin1Char('-'));
            const QString needle = negated ? term.mid(1) : term;
            const bool found = record.url.contains(needle, Qt::CaseInsensitive)
                || record.path.contains(needle, Qt::CaseInsensitive);
            if (found == negated)
                return false;
        }
    }
    return true;
}

} // namespace

void WebExtensionManager::connectDownloads()
{
    if (m_downloadsConnected)
        return;
    m_downloadsConnected = true;

    DownloadManager *manager = DownloadManager::instance();
    connect(manager, &DownloadManager::downloadRecordCreated, this,
            [this](int downloadId) {
                const DownloadManager::Record record =
                    DownloadManager::instance()->downloadRecord(downloadId);
                m_downloadSnapshots.insert(downloadId, recordToJson(record));
                broadcastDownloadEvent(QStringLiteral("onCreated"),
                                       QJsonArray{ m_downloadSnapshots.value(downloadId) });
            });
    connect(manager, &DownloadManager::downloadRecordChanged, this,
            [this](int downloadId) { notifyDownloadChanged(downloadId); });
    connect(manager, &DownloadManager::downloadRecordErased, this,
            [this](int downloadId) {
                m_downloadSnapshots.remove(downloadId);
                broadcastDownloadEvent(QStringLiteral("onErased"), QJsonArray{ downloadId });
            });
}

void WebExtensionManager::notifyDownloadChanged(int downloadId)
{
    const QJsonObject current =
        recordToJson(DownloadManager::instance()->downloadRecord(downloadId));
    const QJsonObject previous = m_downloadSnapshots.value(downloadId);
    m_downloadSnapshots.insert(downloadId, current);

    // onChanged carries a delta, not the item: one entry per field that moved,
    // each { previous, current }. Progress fields are deliberately left out —
    // they change on every network chunk, and Chrome does not report them
    // either.
    static const char *const kFields[] = { "state", "error", "filename", "exists",
                                           "totalBytes", "endTime", "paused", nullptr };
    QJsonObject delta{ { QStringLiteral("id"), downloadId } };
    for (int i = 0; kFields[i]; ++i) {
        const QString field = QLatin1String(kFields[i]);
        const QJsonValue before = previous.value(field);
        const QJsonValue after = current.value(field);
        if (before == after)
            continue;
        delta.insert(field, QJsonObject{ { QStringLiteral("previous"), before },
                                         { QStringLiteral("current"), after } });
    }
    if (delta.size() == 1)
        return; // id only: nothing an extension asked to hear about

    broadcastDownloadEvent(QStringLiteral("onChanged"), QJsonArray{ delta });
}

void WebExtensionManager::broadcastDownloadEvent(const QString &event, const QJsonArray &args)
{
    for (const Entry &entry : m_entries) {
        if (!entry.enabled || (!entry.background && !entry.backgroundView))
            continue;
        if (!entry.extension.hasPermission(QStringLiteral("downloads")))
            continue;
        emitEvent(entry.extension.id(), ExtContext(), QStringLiteral("downloads.") + event, args);
    }
}

bool WebExtensionManager::dispatchDownloadsApi(const QString &extensionId, const ExtContext &origin,
                                               int seq, const QString &api, const QJsonArray &args)
{
    if (!api.startsWith(QLatin1String("downloads.")))
        return false;

    const Entry *entry = entryFor(extensionId);
    if (!entry)
        return true;

    const auto fail = [&](const QString &message) {
        reply(extensionId, origin, seq, false, QJsonValue(), message);
    };
    if (!entry->extension.hasPermission(QStringLiteral("downloads"))) {
        fail(QStringLiteral("%1 requires the \"downloads\" permission").arg(api));
        return true;
    }

    // Events are only wired once an extension that can hear them shows up.
    connectDownloads();

    DownloadManager *manager = DownloadManager::instance();
    const QString method = api.section(QLatin1Char('.'), 1);

    if (method == QLatin1String("download")) {
        const QJsonObject options = args.at(0).toObject();
        const QString url = options.value(QStringLiteral("url")).toString();
        if (url.isEmpty()) {
            fail(QStringLiteral("downloads.download needs a url"));
            return true;
        }

        // A path with directories in it would put the file outside the
        // download folder; the API only promises a name below it, and this is
        // the one place an extension could otherwise write anywhere.
        const QString requested = options.value(QStringLiteral("filename")).toString();
        if (requested.contains(QLatin1Char('/'))) {
            fail(QStringLiteral("filename must be a name, not a path"));
            return true;
        }
        // headers/method/body are not carried: the download is started by the
        // network session, not by a request we build. Saying so beats a silent
        // plain GET.
        if (options.contains(QStringLiteral("headers"))
            || options.value(QStringLiteral("method")).toString()
                   .compare(QLatin1String("POST"), Qt::CaseInsensitive) == 0) {
            fail(QStringLiteral("downloads.download cannot send custom headers or a POST body"));
            return true;
        }

        QString error;
        const int downloadId = manager->startDownload(
            url, requested, options.value(QStringLiteral("saveAs")).toBool(), &error);
        if (downloadId < 0) {
            fail(error);
            return true;
        }
        reply(extensionId, origin, seq, true, downloadId);
        return true;
    }

    if (method == QLatin1String("search") || method == QLatin1String("erase")) {
        const QJsonObject query = args.at(0).toObject();
        const bool erasing = method == QLatin1String("erase");

        QList<DownloadManager::Record> records = manager->downloadRecords();
        // Newest first, which is what "orderBy: -startTime" (the default in
        // practice) asks for. Anything else is not supported.
        std::sort(records.begin(), records.end(),
                  [](const DownloadManager::Record &a, const DownloadManager::Record &b) {
                      return a.startTime > b.startTime;
                  });

        const int limit = query.value(QStringLiteral("limit")).toInt();
        QJsonArray results;
        QJsonArray erased;
        for (const DownloadManager::Record &record : records) {
            if (!matchesQuery(record, query))
                continue;
            if (erasing) {
                if (manager->eraseDownloadRecord(record.id))
                    erased.append(record.id);
                continue;
            }
            results.append(recordToJson(record));
            if (limit > 0 && results.size() >= limit)
                break;
        }
        reply(extensionId, origin, seq, true, erasing ? erased : results);
        return true;
    }

    if (method == QLatin1String("cancel")) {
        const int downloadId = args.at(0).toInt();
        if (manager->downloadRecord(downloadId).id == 0) {
            fail(QStringLiteral("no download with id %1").arg(downloadId));
            return true;
        }
        manager->cancel(downloadId);
        reply(extensionId, origin, seq, true, QJsonValue());
        return true;
    }

    if (method == QLatin1String("removeFile")) {
        QString error;
        if (!manager->removeDownloadFile(args.at(0).toInt(), &error)) {
            fail(error);
            return true;
        }
        reply(extensionId, origin, seq, true, QJsonValue());
        return true;
    }

    fail(QStringLiteral("%1 is not implemented").arg(api));
    return true;
}
