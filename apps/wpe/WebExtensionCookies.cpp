/*
 * Atlantic Browser — browser.cookies for the WebExtension host.
 *
 * Backed by the default network session's WebKitCookieManager. Private tabs
 * run on an ephemeral session that is deliberately not exposed here: there is
 * one cookie store, "0", and a private-mode cookie is neither readable nor
 * writable through this API.
 *
 * Every call is gated twice, as in Chrome and Firefox: the extension must
 * declare the "cookies" permission, and it must have host access to the URL
 * the cookie belongs to. A cookie the extension may not see is filtered out of
 * getAll() rather than failing the call.
 *
 * WebKit reports no cookie changes of its own, so cookies.onChanged fires for
 * mutations made through this API (cause "explicit"/"overwrite") and not for
 * cookies a page sets. That is a real gap, not an oversight — see
 * atlantic-engine/docs/investigations/webextensions.md.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "WebExtensionManager.h"

#include "WPEWebPage.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <memory>

#pragma push_macro("signals")
#undef signals
#include <libsoup/soup.h>
#pragma pop_macro("signals")

namespace {

const char kDefaultStoreId[] = "0";

WebKitCookieManager *defaultCookieManager()
{
    WebKitNetworkSession *session = webkit_network_session_get_default();
    return session ? webkit_network_session_get_cookie_manager(session) : nullptr;
}

QString sameSiteName(SoupSameSitePolicy policy)
{
    switch (policy) {
    case SOUP_SAME_SITE_POLICY_LAX:    return QStringLiteral("lax");
    case SOUP_SAME_SITE_POLICY_STRICT: return QStringLiteral("strict");
    case SOUP_SAME_SITE_POLICY_NONE:   break;
    }
    return QStringLiteral("no_restriction");
}

SoupSameSitePolicy sameSitePolicy(const QString &name)
{
    if (name.compare(QLatin1String("lax"), Qt::CaseInsensitive) == 0)
        return SOUP_SAME_SITE_POLICY_LAX;
    if (name.compare(QLatin1String("strict"), Qt::CaseInsensitive) == 0)
        return SOUP_SAME_SITE_POLICY_STRICT;
    return SOUP_SAME_SITE_POLICY_NONE;
}

QString cookieDomain(SoupCookie *cookie)
{
    const char *domain = soup_cookie_get_domain(cookie);
    return domain ? QString::fromUtf8(domain) : QString();
}

// The URL a cookie belongs to, which is what host permissions are checked
// against: the leading dot of a domain cookie is not part of a host.
QUrl cookieUrl(SoupCookie *cookie)
{
    QString host = cookieDomain(cookie);
    if (host.startsWith(QLatin1Char('.')))
        host.remove(0, 1);
    const char *path = soup_cookie_get_path(cookie);

    QUrl url;
    url.setScheme(soup_cookie_get_secure(cookie) ? QStringLiteral("https")
                                                 : QStringLiteral("http"));
    url.setHost(host);
    url.setPath(path ? QString::fromUtf8(path) : QStringLiteral("/"));
    return url;
}

QJsonObject cookieToJson(SoupCookie *cookie)
{
    const QString domain = cookieDomain(cookie);
    const char *name = soup_cookie_get_name(cookie);
    const char *value = soup_cookie_get_value(cookie);
    const char *path = soup_cookie_get_path(cookie);

    QJsonObject object{
        { QStringLiteral("name"), name ? QString::fromUtf8(name) : QString() },
        { QStringLiteral("value"), value ? QString::fromUtf8(value) : QString() },
        // The API reports the domain without the leading dot and says so with
        // hostOnly instead, which is the opposite of how libsoup stores it.
        { QStringLiteral("domain"),
          domain.startsWith(QLatin1Char('.')) ? domain.mid(1) : domain },
        { QStringLiteral("hostOnly"), !domain.startsWith(QLatin1Char('.')) },
        { QStringLiteral("path"), path ? QString::fromUtf8(path) : QStringLiteral("/") },
        { QStringLiteral("secure"), bool(soup_cookie_get_secure(cookie)) },
        { QStringLiteral("httpOnly"), bool(soup_cookie_get_http_only(cookie)) },
        { QStringLiteral("sameSite"), sameSiteName(soup_cookie_get_same_site_policy(cookie)) },
        { QStringLiteral("storeId"), QLatin1String(kDefaultStoreId) },
        // No container support, and none planned; Firefox reports the same
        // value for an add-on that does not use containers.
        { QStringLiteral("firstPartyDomain"), QString() }
    };

    GDateTime *expires = soup_cookie_get_expires(cookie);
    object.insert(QStringLiteral("session"), expires == nullptr);
    if (expires)
        object.insert(QStringLiteral("expirationDate"), double(g_date_time_to_unix(expires)));
    return object;
}

// details.domain matches the cookie's own domain and any of its subdomains,
// exactly as the API specifies.
bool domainFilterMatches(const QString &filter, const QString &domain)
{
    const QString bare = domain.startsWith(QLatin1Char('.')) ? domain.mid(1) : domain;
    const QString wanted = filter.startsWith(QLatin1Char('.')) ? filter.mid(1) : filter;
    return bare.compare(wanted, Qt::CaseInsensitive) == 0
        || bare.endsWith(QLatin1Char('.') + wanted, Qt::CaseInsensitive);
}

bool cookieMatchesFilter(SoupCookie *cookie, const QJsonObject &details)
{
    const auto has = [&details](const char *key) {
        return details.contains(QLatin1String(key)) && !details.value(QLatin1String(key)).isNull();
    };

    if (has("name")) {
        const char *name = soup_cookie_get_name(cookie);
        if (details.value(QStringLiteral("name")).toString()
            != (name ? QString::fromUtf8(name) : QString())) {
            return false;
        }
    }
    if (has("domain")
        && !domainFilterMatches(details.value(QStringLiteral("domain")).toString(),
                                cookieDomain(cookie))) {
        return false;
    }
    if (has("path")) {
        const char *path = soup_cookie_get_path(cookie);
        if (details.value(QStringLiteral("path")).toString()
            != (path ? QString::fromUtf8(path) : QString())) {
            return false;
        }
    }
    if (has("secure")
        && details.value(QStringLiteral("secure")).toBool() != bool(soup_cookie_get_secure(cookie))) {
        return false;
    }
    if (has("session")
        && details.value(QStringLiteral("session")).toBool()
               != (soup_cookie_get_expires(cookie) == nullptr)) {
        return false;
    }
    if (has("url")) {
        const QByteArray uri = details.value(QStringLiteral("url")).toString().toUtf8();
        GUri *parsed = g_uri_parse(uri.constData(), G_URI_FLAGS_NONE, nullptr);
        if (!parsed)
            return false;
        const bool applies = soup_cookie_applies_to_uri(cookie, parsed);
        g_uri_unref(parsed);
        if (!applies)
            return false;
    }
    return true;
}

// Reply address for an async cookie call. ExtContext is private to the
// manager, so the origin travels as its two fields, the same way the
// executeScript callback carries it.
struct CookieCallback {
    QPointer<WebExtensionManager> manager;
    QString extensionId;
    WPEWebPage *originPage = nullptr;
    bool originMainWorld = false;
    int seq = 0;
    QJsonObject details;
    // get/getAll only want the first match; remove needs the cookie itself.
    bool single = false;
    bool removing = false;
};

} // namespace

// Shared by cookies.get, cookies.getAll and the read half of cookies.remove.
void WebExtensionManager::finishCookieQuery(void *userData, void *listPtr, const QString &error)
{
    std::unique_ptr<CookieCallback> callback(static_cast<CookieCallback *>(userData));
    GList *cookies = static_cast<GList *>(listPtr);

    if (!callback->manager) {
        g_list_free_full(cookies, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
        return;
    }

    WebExtensionManager *manager = callback->manager;
    const ExtContext origin{ callback->originPage, callback->originMainWorld };
    const Entry *entry = manager->entryFor(callback->extensionId);
    if (!entry) {
        g_list_free_full(cookies, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
        return;
    }

    if (!error.isEmpty()) {
        manager->reply(callback->extensionId, origin, callback->seq, false, QJsonValue(), error);
        return;
    }

    SoupCookie *removeTarget = nullptr;
    QJsonArray matches;
    for (GList *item = cookies; item; item = item->next) {
        SoupCookie *cookie = static_cast<SoupCookie *>(item->data);
        if (!cookie)
            continue;
        // A cookie the extension has no host access to does not exist as far
        // as it is concerned; filtering beats failing the whole call.
        if (!entry->extension.hasHostAccess(cookieUrl(cookie)))
            continue;
        if (!cookieMatchesFilter(cookie, callback->details))
            continue;

        if (callback->removing && !removeTarget) {
            removeTarget = soup_cookie_copy(cookie);
            break;
        }
        matches.append(cookieToJson(cookie));
        if (callback->single)
            break;
    }
    g_list_free_full(cookies, reinterpret_cast<GDestroyNotify>(soup_cookie_free));

    if (callback->removing) {
        if (!removeTarget) {
            // Nothing to remove is not an error; the API resolves with null.
            manager->reply(callback->extensionId, origin, callback->seq, true, QJsonValue());
            return;
        }
        manager->deleteCookie(callback->extensionId, origin, callback->seq, removeTarget);
        return;
    }

    if (callback->single) {
        manager->reply(callback->extensionId, origin, callback->seq, true,
                       matches.isEmpty() ? QJsonValue() : matches.at(0));
        return;
    }
    manager->reply(callback->extensionId, origin, callback->seq, true, matches);
}

void WebExtensionManager::deleteCookie(const QString &extensionId, const ExtContext &origin,
                                       int seq, void *cookiePtr)
{
    SoupCookie *cookie = static_cast<SoupCookie *>(cookiePtr);
    WebKitCookieManager *cookieManager = defaultCookieManager();
    if (!cookieManager) {
        soup_cookie_free(cookie);
        reply(extensionId, origin, seq, false, QJsonValue(),
              QStringLiteral("no cookie store is available"));
        return;
    }

    // What the callback has to answer with, captured while the cookie is still
    // alive: remove() resolves with the cookie's details, and onChanged carries
    // the whole cookie.
    QJsonObject removed = cookieToJson(cookie);
    removed.insert(QStringLiteral("_url"), cookieUrl(cookie).toString());

    webkit_cookie_manager_delete_cookie(
        cookieManager, cookie, nullptr,
        +[](GObject *object, GAsyncResult *result, gpointer userData) {
            std::unique_ptr<CookieCallback> callback(static_cast<CookieCallback *>(userData));
            GError *error = nullptr;
            const gboolean deleted = webkit_cookie_manager_delete_cookie_finish(
                WEBKIT_COOKIE_MANAGER(object), result, &error);
            if (!callback->manager) {
                if (error)
                    g_error_free(error);
                return;
            }
            const ExtContext origin{ callback->originPage, callback->originMainWorld };
            if (!deleted) {
                const QString message = error ? QString::fromUtf8(error->message)
                                              : QStringLiteral("the cookie could not be removed");
                if (error)
                    g_error_free(error);
                callback->manager->reply(callback->extensionId, origin, callback->seq, false,
                                         QJsonValue(), message);
                return;
            }
            if (error)
                g_error_free(error);

            callback->manager->notifyCookieChanged(callback->details, true,
                                                   QStringLiteral("explicit"));
            callback->manager->reply(
                callback->extensionId, origin, callback->seq, true,
                QJsonObject{
                    { QStringLiteral("url"), callback->details.value(QStringLiteral("_url")) },
                    { QStringLiteral("name"), callback->details.value(QStringLiteral("name")) },
                    { QStringLiteral("storeId"), QLatin1String(kDefaultStoreId) } });
        },
        new CookieCallback{ this, extensionId, origin.page, origin.mainWorld, seq, removed,
                            false, false });
    // delete_cookie converts to a WebCore::Cookie before it returns, so the
    // SoupCookie is ours to free here (same for add_cookie below).
    soup_cookie_free(cookie);
}

void WebExtensionManager::notifyCookieChanged(const QJsonObject &cookie, bool removed,
                                              const QString &cause)
{
    QJsonObject payload = cookie;
    payload.remove(QStringLiteral("_url"));

    // Not a broadcast: an extension only hears about a cookie it could have
    // read in the first place.
    QString host = payload.value(QStringLiteral("domain")).toString();
    if (host.startsWith(QLatin1Char('.')))
        host.remove(0, 1);
    QUrl url;
    url.setScheme(payload.value(QStringLiteral("secure")).toBool() ? QStringLiteral("https")
                                                                   : QStringLiteral("http"));
    url.setHost(host);
    url.setPath(payload.value(QStringLiteral("path")).toString());

    const QJsonArray args{ QJsonObject{ { QStringLiteral("removed"), removed },
                                        { QStringLiteral("cookie"), payload },
                                        { QStringLiteral("cause"), cause } } };
    for (const Entry &entry : m_entries) {
        if (!entry.enabled || (!entry.background && !entry.backgroundView))
            continue;
        if (!entry.extension.hasPermission(QStringLiteral("cookies"))
            || !entry.extension.hasHostAccess(url)) {
            continue;
        }
        emitEvent(entry.extension.id(), ExtContext(), QStringLiteral("cookies.onChanged"), args);
    }
}

bool WebExtensionManager::dispatchCookiesApi(const QString &extensionId, const ExtContext &origin,
                                             int seq, const QString &api, const QJsonArray &args)
{
    if (!api.startsWith(QLatin1String("cookies.")))
        return false;

    const Entry *entry = entryFor(extensionId);
    if (!entry)
        return true;

    const auto fail = [&](const QString &message) {
        reply(extensionId, origin, seq, false, QJsonValue(), message);
    };

    if (!entry->extension.hasPermission(QStringLiteral("cookies"))) {
        fail(QStringLiteral("%1 requires the \"cookies\" permission").arg(api));
        return true;
    }

    WebKitCookieManager *cookieManager = defaultCookieManager();
    if (!cookieManager) {
        fail(QStringLiteral("no cookie store is available"));
        return true;
    }

    const QJsonObject details = args.at(0).toObject();
    const QString method = api.section(QLatin1Char('.'), 1);

    // One private store exists (private browsing), and it is deliberately not
    // reachable: an extension asking for it gets an error, not silence.
    const QString storeId = details.value(QStringLiteral("storeId")).toString();
    if (!storeId.isEmpty() && storeId != QLatin1String(kDefaultStoreId)) {
        fail(QStringLiteral("unknown cookie store \"%1\"; Atlantic exposes only \"0\"")
                 .arg(storeId));
        return true;
    }

    if (method == QLatin1String("getAllCookieStores")) {
        QJsonArray tabIds;
        for (WPEWebPage *page : m_pageOrder) {
            if (page && !page->privateBrowsing())
                tabIds.append(page->tabId());
        }
        reply(extensionId, origin, seq, true,
              QJsonArray{ QJsonObject{ { QStringLiteral("id"), QLatin1String(kDefaultStoreId) },
                                       { QStringLiteral("tabIds"), tabIds },
                                       { QStringLiteral("incognito"), false } } });
        return true;
    }

    if (method == QLatin1String("get") || method == QLatin1String("getAll")
        || method == QLatin1String("remove")) {
        const bool single = method != QLatin1String("getAll");
        const QString url = details.value(QStringLiteral("url")).toString();

        if (single && url.isEmpty()) {
            fail(QStringLiteral("cookies.%1 needs a url").arg(method));
            return true;
        }
        if (!url.isEmpty() && !entry->extension.hasHostAccess(QUrl(url))) {
            fail(QStringLiteral("no host permission for %1").arg(url));
            return true;
        }

        auto *callback = new CookieCallback{ this,   extensionId, origin.page,
                                             origin.mainWorld, seq, details,
                                             single, method == QLatin1String("remove") };

        // Two entry points, two finish() functions: which one to call is
        // decided here, where we know which get() was started, and not
        // guessed inside a shared callback.
        if (url.isEmpty()) {
            webkit_cookie_manager_get_all_cookies(
                cookieManager, nullptr,
                +[](GObject *object, GAsyncResult *result, gpointer userData) {
                    GError *error = nullptr;
                    GList *cookies = webkit_cookie_manager_get_all_cookies_finish(
                        WEBKIT_COOKIE_MANAGER(object), result, &error);
                    QString message;
                    if (error) {
                        message = QString::fromUtf8(error->message);
                        g_error_free(error);
                    }
                    WebExtensionManager::finishCookieQuery(userData, cookies, message);
                },
                callback);
        } else {
            webkit_cookie_manager_get_cookies(
                cookieManager, url.toUtf8().constData(), nullptr,
                +[](GObject *object, GAsyncResult *result, gpointer userData) {
                    GError *error = nullptr;
                    GList *cookies = webkit_cookie_manager_get_cookies_finish(
                        WEBKIT_COOKIE_MANAGER(object), result, &error);
                    QString message;
                    if (error) {
                        message = QString::fromUtf8(error->message);
                        g_error_free(error);
                    }
                    WebExtensionManager::finishCookieQuery(userData, cookies, message);
                },
                callback);
        }
        return true;
    }

    if (method == QLatin1String("set")) {
        const QUrl url(details.value(QStringLiteral("url")).toString());
        if (!url.isValid() || url.host().isEmpty()) {
            fail(QStringLiteral("cookies.set needs a valid url"));
            return true;
        }
        if (!entry->extension.hasHostAccess(url)) {
            fail(QStringLiteral("no host permission for %1").arg(url.toString()));
            return true;
        }

        QString domain = details.value(QStringLiteral("domain")).toString();
        if (domain.isEmpty())
            domain = url.host();
        else if (!domain.startsWith(QLatin1Char('.')))
            domain.prepend(QLatin1Char('.')); // an explicit domain is a domain cookie

        QString path = details.value(QStringLiteral("path")).toString();
        if (path.isEmpty()) {
            path = url.path();
            const int slash = path.lastIndexOf(QLatin1Char('/'));
            path = slash > 0 ? path.left(slash) : QStringLiteral("/");
        }

        const QByteArray name = details.value(QStringLiteral("name")).toString().toUtf8();
        const QByteArray value = details.value(QStringLiteral("value")).toString().toUtf8();
        SoupCookie *cookie = soup_cookie_new(name.constData(), value.constData(),
                                             domain.toUtf8().constData(),
                                             path.toUtf8().constData(), -1);
        if (!cookie) {
            fail(QStringLiteral("the cookie could not be created"));
            return true;
        }

        if (details.contains(QStringLiteral("expirationDate"))) {
            const gint64 seconds = gint64(details.value(QStringLiteral("expirationDate")).toDouble());
            if (GDateTime *expires = g_date_time_new_from_unix_utc(seconds)) {
                soup_cookie_set_expires(cookie, expires);
                g_date_time_unref(expires);
            }
        }
        soup_cookie_set_secure(cookie, details.value(QStringLiteral("secure")).toBool());
        soup_cookie_set_http_only(cookie, details.value(QStringLiteral("httpOnly")).toBool());
        soup_cookie_set_same_site_policy(
            cookie, sameSitePolicy(details.value(QStringLiteral("sameSite")).toString()));

        const QJsonObject written = cookieToJson(cookie);
        webkit_cookie_manager_add_cookie(
            cookieManager, cookie, nullptr,
            +[](GObject *object, GAsyncResult *result, gpointer userData) {
                std::unique_ptr<CookieCallback> callback(static_cast<CookieCallback *>(userData));
                GError *error = nullptr;
                const gboolean added = webkit_cookie_manager_add_cookie_finish(
                    WEBKIT_COOKIE_MANAGER(object), result, &error);
                if (!callback->manager) {
                    if (error)
                        g_error_free(error);
                    return;
                }
                const ExtContext origin{ callback->originPage, callback->originMainWorld };
                if (!added) {
                    const QString message = error ? QString::fromUtf8(error->message)
                                                  : QStringLiteral("the cookie could not be set");
                    if (error)
                        g_error_free(error);
                    callback->manager->reply(callback->extensionId, origin, callback->seq, false,
                                             QJsonValue(), message);
                    return;
                }
                if (error)
                    g_error_free(error);

                // "overwrite" is the cause a real browser reports when a set
                // replaces an existing cookie; we cannot tell, and "explicit"
                // is the safe answer for a caller keying off it.
                callback->manager->notifyCookieChanged(callback->details, false,
                                                       QStringLiteral("explicit"));
                callback->manager->reply(callback->extensionId, origin, callback->seq, true,
                                         callback->details);
            },
            new CookieCallback{ this, extensionId, origin.page, origin.mainWorld, seq, written,
                                false, false });
        soup_cookie_free(cookie);
        return true;
    }

    fail(QStringLiteral("%1 is not implemented").arg(api));
    return true;
}
