/*
 * Atlantic Browser — browser.history and browser.bookmarks for the
 * WebExtension host.
 *
 * Both are wired to the browser's own subsystems rather than to a store of
 * their own: history goes through DBManager/DBWorker (the same SQLite the
 * history page reads), bookmarks through the live DeclarativeBookmarkModel, so
 * a bookmark an extension creates shows up in the UI immediately instead of
 * being overwritten by the next save.
 *
 * Neither namespace takes host permissions — as in Chrome and Firefox, the
 * "history" / "bookmarks" permission covers the whole list.
 *
 * Two shapes do not survive the trip, and are documented rather than faked:
 * Atlantic records one row per URL with a visit count and the time of the last
 * visit, so history.getVisits() reports a single visit; and bookmarks are a
 * flat list, presented here as one fixed folder under the root.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "WebExtensionManager.h"

#include "WPEWebPage.h"
#include "dbmanager.h"
#include "declarativebookmarkmodel.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <QVariantMap>

namespace {

// The API insists on a tree. Atlantic has a flat list, so it gets a root and
// one folder, with ids matching the shape Firefox uses (extensions do compare
// against these).
const char kRootId[] = "root________";
const char kFolderId[] = "toolbar_____";

QJsonObject bookmarkNode(const QString &id, const QString &title, const QString &url, int index)
{
    QJsonObject node{
        { QStringLiteral("id"), id },
        { QStringLiteral("parentId"), QLatin1String(kFolderId) },
        { QStringLiteral("index"), index },
        { QStringLiteral("title"), title },
        { QStringLiteral("type"), QStringLiteral("bookmark") },
        // Not recorded by the bookmark store; reporting 0 is honest, and the
        // field has to exist for callers that read it blindly.
        { QStringLiteral("dateAdded"), 0 }
    };
    if (!url.isEmpty())
        node.insert(QStringLiteral("url"), url);
    return node;
}

QJsonObject folderNode(const QJsonArray &children)
{
    return QJsonObject{ { QStringLiteral("id"), QLatin1String(kFolderId) },
                        { QStringLiteral("parentId"), QLatin1String(kRootId) },
                        { QStringLiteral("index"), 0 },
                        { QStringLiteral("title"), QStringLiteral("Bookmarks") },
                        { QStringLiteral("type"), QStringLiteral("folder") },
                        { QStringLiteral("dateAdded"), 0 },
                        { QStringLiteral("children"), children } };
}

} // namespace

// --- browser.history ---------------------------------------------------------

void WebExtensionManager::connectHistory()
{
    if (m_historyConnected)
        return;
    m_historyConnected = true;
    connect(DBManager::instance(), &DBManager::historySearchAvailable,
            this, &WebExtensionManager::onHistorySearchAvailable);
}

void WebExtensionManager::onHistorySearchAvailable(int requestId, const QVariantList &entries)
{
    const auto pending = m_historyRequests.find(requestId);
    if (pending == m_historyRequests.end())
        return;
    const HistoryRequest request = pending.value();
    m_historyRequests.erase(pending);

    const ExtContext origin{ request.page, request.mainWorld };

    QJsonArray results;
    for (const QVariant &value : entries) {
        const QVariantMap entry = value.toMap();
        const QString url = entry.value(QStringLiteral("url")).toString();
        // getVisits() asks about one URL; searchHistory has no exact-match
        // mode, so the LIKE result is narrowed here.
        if (!request.url.isEmpty() && url != request.url)
            continue;

        const qint64 lastVisit = entry.value(QStringLiteral("lastVisitTime")).toLongLong();
        const int visitCount = entry.value(QStringLiteral("visitCount")).toInt();

        if (request.mode == QLatin1String("getVisits")) {
            // One row per URL is all Atlantic stores: report the last visit and
            // say how many there were, rather than inventing the ones in
            // between with made-up timestamps.
            results.append(QJsonObject{
                { QStringLiteral("id"), QString::number(entry.value(QStringLiteral("id")).toInt()) },
                { QStringLiteral("visitId"),
                  QString::number(entry.value(QStringLiteral("id")).toInt()) },
                { QStringLiteral("visitTime"), double(lastVisit) },
                { QStringLiteral("referringVisitId"), QStringLiteral("0") },
                { QStringLiteral("transition"), QStringLiteral("link") } });
            continue;
        }

        results.append(QJsonObject{
            { QStringLiteral("id"), QString::number(entry.value(QStringLiteral("id")).toInt()) },
            { QStringLiteral("url"), url },
            { QStringLiteral("title"), entry.value(QStringLiteral("title")).toString() },
            { QStringLiteral("lastVisitTime"), double(lastVisit) },
            { QStringLiteral("visitCount"), visitCount },
            { QStringLiteral("typedCount"), 0 } });
    }

    reply(request.extensionId, origin, request.seq, true, results);
}

void WebExtensionManager::notifyHistoryVisit(int tabId, const QString &url, const QString &title)
{
    Q_UNUSED(tabId);
    if (url.isEmpty() || url.startsWith(QLatin1String("about:")))
        return;

    for (const Entry &entry : m_entries) {
        if (!entry.enabled || (!entry.background && !entry.backgroundView))
            continue;
        if (!entry.extension.hasPermission(QStringLiteral("history")))
            continue;
        emitEvent(entry.extension.id(), ExtContext(), QStringLiteral("history.onVisited"),
                  QJsonArray{ QJsonObject{
                      { QStringLiteral("id"), QString() },
                      { QStringLiteral("url"), url },
                      { QStringLiteral("title"), title },
                      { QStringLiteral("lastVisitTime"),
                        double(QDateTime::currentMSecsSinceEpoch()) } } });
    }
}

bool WebExtensionManager::dispatchHistoryApi(const QString &extensionId, const ExtContext &origin,
                                             int seq, const QString &api, const QJsonArray &args)
{
    if (!api.startsWith(QLatin1String("history.")))
        return false;

    const Entry *entry = entryFor(extensionId);
    if (!entry)
        return true;

    const auto fail = [&](const QString &message) {
        reply(extensionId, origin, seq, false, QJsonValue(), message);
    };
    if (!entry->extension.hasPermission(QStringLiteral("history"))) {
        fail(QStringLiteral("%1 requires the \"history\" permission").arg(api));
        return true;
    }

    const QString method = api.section(QLatin1Char('.'), 1);
    const QJsonObject details = args.at(0).toObject();

    if (method == QLatin1String("search") || method == QLatin1String("getVisits")) {
        const QString url = details.value(QStringLiteral("url")).toString();
        const bool visits = method == QLatin1String("getVisits");
        if (visits && url.isEmpty()) {
            fail(QStringLiteral("history.getVisits needs a url"));
            return true;
        }

        connectHistory();
        const int requestId = m_nextHistoryRequest++;
        m_historyRequests.insert(requestId,
                                 HistoryRequest{ extensionId, origin.page, origin.mainWorld, seq,
                                                 method, visits ? url : QString() });

        // maxResults defaults to 100, as in the API; a getVisits lookup only
        // ever wants the one row that matches the URL.
        const int maxResults = visits
            ? 1
            : (details.contains(QStringLiteral("maxResults"))
                   ? details.value(QStringLiteral("maxResults")).toInt()
                   : 100);
        DBManager::instance()->searchHistory(
            requestId, visits ? url : details.value(QStringLiteral("text")).toString(),
            qint64(details.value(QStringLiteral("startTime")).toDouble()),
            qint64(details.value(QStringLiteral("endTime")).toDouble()), maxResults);
        return true;
    }

    if (method == QLatin1String("addUrl")) {
        const QString url = details.value(QStringLiteral("url")).toString();
        if (url.isEmpty()) {
            fail(QStringLiteral("history.addUrl needs a url"));
            return true;
        }
        DBManager::instance()->addHistoryEntry(url, details.value(QStringLiteral("title")).toString());
        reply(extensionId, origin, seq, true, QJsonValue());
        return true;
    }

    if (method == QLatin1String("deleteUrl")) {
        const QString url = details.value(QStringLiteral("url")).toString();
        if (url.isEmpty()) {
            fail(QStringLiteral("history.deleteUrl needs a url"));
            return true;
        }
        DBManager::instance()->removeHistoryEntry(url);
        broadcastHistoryRemoved(false, QJsonArray{ url });
        reply(extensionId, origin, seq, true, QJsonValue());
        return true;
    }

    if (method == QLatin1String("deleteRange")) {
        DBManager::instance()->deleteHistoryRange(
            qint64(details.value(QStringLiteral("startTime")).toDouble()),
            qint64(details.value(QStringLiteral("endTime")).toDouble()));
        broadcastHistoryRemoved(false, QJsonArray());
        reply(extensionId, origin, seq, true, QJsonValue());
        return true;
    }

    if (method == QLatin1String("deleteAll")) {
        DBManager::instance()->clearHistory(0);
        broadcastHistoryRemoved(true, QJsonArray());
        reply(extensionId, origin, seq, true, QJsonValue());
        return true;
    }

    fail(QStringLiteral("%1 is not implemented").arg(api));
    return true;
}

void WebExtensionManager::broadcastHistoryRemoved(bool allHistory, const QJsonArray &urls)
{
    for (const Entry &entry : m_entries) {
        if (!entry.enabled || (!entry.background && !entry.backgroundView))
            continue;
        if (!entry.extension.hasPermission(QStringLiteral("history")))
            continue;
        emitEvent(entry.extension.id(), ExtContext(), QStringLiteral("history.onVisitRemoved"),
                  QJsonArray{ QJsonObject{ { QStringLiteral("allHistory"), allHistory },
                                           { QStringLiteral("urls"), urls } } });
    }
}

// --- browser.bookmarks -------------------------------------------------------

QString WebExtensionManager::bookmarkIdFor(const QString &url)
{
    for (auto it = m_bookmarkIds.constBegin(); it != m_bookmarkIds.constEnd(); ++it) {
        if (it.value() == url)
            return it.key();
    }
    // Ids only have to be opaque and stable for as long as the extension is
    // running; they are handed back out in every node we report.
    const QString id = QStringLiteral("atl-bm-%1").arg(m_bookmarkIds.size() + 1);
    m_bookmarkIds.insert(id, url);
    return id;
}

QJsonArray WebExtensionManager::bookmarkNodes()
{
    QJsonArray nodes;
    DeclarativeBookmarkModel *model = DeclarativeBookmarkModel::primaryInstance();
    if (!model)
        return nodes;

    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        const QString url = model->data(index, DeclarativeBookmarkModel::UrlRole).toString();
        const QString title = model->data(index, DeclarativeBookmarkModel::TitleRole).toString();
        nodes.append(bookmarkNode(bookmarkIdFor(url), title, url, row));
    }
    return nodes;
}

void WebExtensionManager::broadcastBookmarkEvent(const QString &event, const QJsonArray &args)
{
    for (const Entry &entry : m_entries) {
        if (!entry.enabled || (!entry.background && !entry.backgroundView))
            continue;
        if (!entry.extension.hasPermission(QStringLiteral("bookmarks")))
            continue;
        emitEvent(entry.extension.id(), ExtContext(), QStringLiteral("bookmarks.") + event, args);
    }
}

bool WebExtensionManager::dispatchBookmarksApi(const QString &extensionId, const ExtContext &origin,
                                               int seq, const QString &api, const QJsonArray &args)
{
    if (!api.startsWith(QLatin1String("bookmarks.")))
        return false;

    const Entry *entry = entryFor(extensionId);
    if (!entry)
        return true;

    const auto fail = [&](const QString &message) {
        reply(extensionId, origin, seq, false, QJsonValue(), message);
    };
    if (!entry->extension.hasPermission(QStringLiteral("bookmarks"))) {
        fail(QStringLiteral("%1 requires the \"bookmarks\" permission").arg(api));
        return true;
    }

    DeclarativeBookmarkModel *model = DeclarativeBookmarkModel::primaryInstance();
    if (!model) {
        fail(QStringLiteral("the bookmark list is not available yet"));
        return true;
    }

    const QString method = api.section(QLatin1Char('.'), 1);
    const QJsonArray nodes = bookmarkNodes();

    const auto nodeById = [&nodes](const QString &id) {
        for (const QJsonValue &value : nodes) {
            if (value.toObject().value(QStringLiteral("id")).toString() == id)
                return value.toObject();
        }
        return QJsonObject();
    };

    if (method == QLatin1String("getTree") || method == QLatin1String("getSubTree")) {
        const QJsonObject root{ { QStringLiteral("id"), QLatin1String(kRootId) },
                                { QStringLiteral("title"), QString() },
                                { QStringLiteral("type"), QStringLiteral("folder") },
                                { QStringLiteral("dateAdded"), 0 },
                                { QStringLiteral("children"), QJsonArray{ folderNode(nodes) } } };
        if (method == QLatin1String("getSubTree")) {
            const QString id = args.at(0).toString();
            if (id == QLatin1String(kFolderId)) {
                reply(extensionId, origin, seq, true, QJsonArray{ folderNode(nodes) });
                return true;
            }
            if (id != QLatin1String(kRootId)) {
                const QJsonObject node = nodeById(id);
                if (node.isEmpty()) {
                    fail(QStringLiteral("no bookmark with id %1").arg(id));
                    return true;
                }
                reply(extensionId, origin, seq, true, QJsonArray{ node });
                return true;
            }
        }
        reply(extensionId, origin, seq, true, QJsonArray{ root });
        return true;
    }

    if (method == QLatin1String("getChildren")) {
        const QString id = args.at(0).toString();
        if (id == QLatin1String(kRootId)) {
            reply(extensionId, origin, seq, true, QJsonArray{ folderNode(QJsonArray()) });
            return true;
        }
        reply(extensionId, origin, seq, true,
              id == QLatin1String(kFolderId) ? nodes : QJsonArray());
        return true;
    }

    if (method == QLatin1String("get")) {
        QStringList ids;
        if (args.at(0).isArray()) {
            const QJsonArray requested = args.at(0).toArray();
            for (const QJsonValue &value : requested)
                ids << value.toString();
        } else {
            ids << args.at(0).toString();
        }

        QJsonArray found;
        for (const QString &id : ids) {
            if (id == QLatin1String(kFolderId)) {
                found.append(folderNode(QJsonArray()));
                continue;
            }
            const QJsonObject node = nodeById(id);
            if (!node.isEmpty())
                found.append(node);
        }
        if (found.isEmpty()) {
            fail(QStringLiteral("no bookmark with that id"));
            return true;
        }
        reply(extensionId, origin, seq, true, found);
        return true;
    }

    if (method == QLatin1String("search")) {
        // The API takes either a string or { query, url, title }.
        QString query;
        QString url;
        QString title;
        if (args.at(0).isString()) {
            query = args.at(0).toString();
        } else {
            const QJsonObject object = args.at(0).toObject();
            query = object.value(QStringLiteral("query")).toString();
            url = object.value(QStringLiteral("url")).toString();
            title = object.value(QStringLiteral("title")).toString();
        }

        QJsonArray matches;
        for (const QJsonValue &value : nodes) {
            const QJsonObject node = value.toObject();
            const QString nodeUrl = node.value(QStringLiteral("url")).toString();
            const QString nodeTitle = node.value(QStringLiteral("title")).toString();
            if (!url.isEmpty() && nodeUrl != url)
                continue;
            if (!title.isEmpty() && nodeTitle != title)
                continue;
            if (!query.isEmpty()
                && !nodeUrl.contains(query, Qt::CaseInsensitive)
                && !nodeTitle.contains(query, Qt::CaseInsensitive)) {
                continue;
            }
            matches.append(node);
        }
        reply(extensionId, origin, seq, true, matches);
        return true;
    }

    if (method == QLatin1String("create")) {
        const QJsonObject details = args.at(0).toObject();
        const QString url = details.value(QStringLiteral("url")).toString();
        if (url.isEmpty()) {
            // A flat list has nowhere to put a folder, and silently creating a
            // bookmark instead would be worse than saying so.
            fail(QStringLiteral("Atlantic's bookmarks are a flat list; only bookmarks with a "
                                "url can be created"));
            return true;
        }
        const QString title = details.value(QStringLiteral("title")).toString();
        model->add(url, title.isEmpty() ? url : title, QString(), false);

        const QJsonObject node = bookmarkNode(bookmarkIdFor(url), title, url,
                                              model->rowCount() - 1);
        broadcastBookmarkEvent(QStringLiteral("onCreated"),
                               QJsonArray{ node.value(QStringLiteral("id")), node });
        reply(extensionId, origin, seq, true, node);
        return true;
    }

    if (method == QLatin1String("remove") || method == QLatin1String("removeTree")) {
        const QString id = args.at(0).toString();
        const QJsonObject node = nodeById(id);
        if (node.isEmpty()) {
            fail(QStringLiteral("no bookmark with id %1").arg(id));
            return true;
        }
        const QString url = node.value(QStringLiteral("url")).toString();
        model->remove(url);
        m_bookmarkIds.remove(id);
        broadcastBookmarkEvent(
            QStringLiteral("onRemoved"),
            QJsonArray{ id,
                        QJsonObject{ { QStringLiteral("parentId"), QLatin1String(kFolderId) },
                                     { QStringLiteral("index"),
                                       node.value(QStringLiteral("index")) },
                                     { QStringLiteral("node"), node } } });
        reply(extensionId, origin, seq, true, QJsonValue());
        return true;
    }

    if (method == QLatin1String("update")) {
        const QString id = args.at(0).toString();
        const QJsonObject changes = args.at(1).toObject();
        const QJsonObject node = nodeById(id);
        if (node.isEmpty()) {
            fail(QStringLiteral("no bookmark with id %1").arg(id));
            return true;
        }

        const QString oldUrl = node.value(QStringLiteral("url")).toString();
        const int row = model->indexOfUrl(oldUrl);
        if (row < 0) {
            fail(QStringLiteral("no bookmark with id %1").arg(id));
            return true;
        }
        const QString newUrl = changes.contains(QStringLiteral("url"))
            ? changes.value(QStringLiteral("url")).toString()
            : oldUrl;
        const QString newTitle = changes.contains(QStringLiteral("title"))
            ? changes.value(QStringLiteral("title")).toString()
            : node.value(QStringLiteral("title")).toString();
        model->edit(row, newUrl, newTitle);
        m_bookmarkIds.insert(id, newUrl);

        const QJsonObject updated = bookmarkNode(id, newTitle, newUrl,
                                                 node.value(QStringLiteral("index")).toInt());
        broadcastBookmarkEvent(
            QStringLiteral("onChanged"),
            QJsonArray{ id,
                        QJsonObject{ { QStringLiteral("title"), newTitle },
                                     { QStringLiteral("url"), newUrl } } });
        reply(extensionId, origin, seq, true, updated);
        return true;
    }

    if (method == QLatin1String("getRecent")) {
        // No creation timestamps to sort by; the list's own order is the best
        // answer available, newest last.
        const int count = qMax(1, args.at(0).toInt());
        QJsonArray recent;
        for (int i = nodes.size() - 1; i >= 0 && recent.size() < count; --i)
            recent.append(nodes.at(i));
        reply(extensionId, origin, seq, true, recent);
        return true;
    }

    fail(QStringLiteral("%1 is not implemented").arg(api));
    return true;
}
