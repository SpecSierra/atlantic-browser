/*
 * Copyright (c) 2026
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "AdBlockEngine.h"
#include "AdBlockListUpdater.h"
#include "WPEWebPage.h"
#include "WPEUserScripts.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QRunnable>
#include <QSet>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVector>

bool AdBlockEngine::s_enabled = true;
QStringList AdBlockEngine::s_allowlist;

namespace {

const char kShippedListDir[] = "/usr/share/atlantic-browser";
// Upper bound on a caller's wait for the initial load. The load takes a
// fraction of a second; this only keeps a load that never finishes from
// wedging the GUI thread.
const unsigned long kLoadWaitMs = 5000;

} // namespace

// Qt 5.6 has no QRunnable::create(), hence a class; a friend so load() can
// stay private.
class AdBlockEngineLoader : public QRunnable
{
public:
    AdBlockEngineLoader() { setAutoDelete(true); }
    void run() override { AdBlockEngine::instance().load(); }
};

AdBlockEngine& AdBlockEngine::instance()
{
    static AdBlockEngine inst;
    return inst;
}

AdBlockEngine::~AdBlockEngine()
{
    {
        // Never free the engine out from under a load that is still running.
        QMutexLocker locker(&m_loadMutex);
        if (m_loadStarted && !m_loadSettled)
            m_loadDone.wait(&m_loadMutex, kLoadWaitMs);
    }
    if (m_ready.loadAcquire() && m_engine)
        atlantic_adblock_destroy(m_engine);
}

void AdBlockEngine::startLoading()
{
    QMutexLocker locker(&m_loadMutex);
    if (m_loadStarted)
        return;
    m_loadStarted = true;
    QThreadPool::globalInstance()->start(new AdBlockEngineLoader);
}

void AdBlockEngine::load()
{
    QElapsedTimer timer;
    timer.start();

    // Prefer whichever of the shipped copy and the updater's downloaded copy
    // carries the higher engine.version stamp (the WebProcess extension
    // applies the same rule).
    const QString shipped = QLatin1String(kShippedListDir);
    QString dir = shipped;
    const QString updated = AdBlockListUpdater::cacheDir();
    if (QFileInfo::exists(updated + QStringLiteral("/engine.dat"))
        && AdBlockListUpdater::versionIn(updated) > AdBlockListUpdater::versionIn(dir)) {
        dir = updated;
        qInfo() << "[ADBLOCK] using updated lists, version" << AdBlockListUpdater::versionIn(updated);
    }
    AtlanticAdblockEngine* engine = createFromCache(dir + QStringLiteral("/engine.dat"));
    if (!engine && dir != shipped) {
        // A downloaded engine that will not load must not take blocking down
        // with it: fall back to the shipped copy (as the WebProcess extension
        // does) and drop the bad stamp so the next start does not choose it
        // again.
        qWarning() << "[ADBLOCK] updated engine unusable; using the shipped copy";
        QFile::remove(updated + QStringLiteral("/engine.version"));
        dir = shipped;
        engine = createFromCache(dir + QStringLiteral("/engine.dat"));
    }
    if (!engine) {
        qWarning() << "[ADBLOCK] engine not available — blocking is off";
    } else {
        // Scriptlet resources live next to the engine cache; without them every
        // ##+js(...) rule is a no-op. Loaded before publishing, so no caller can
        // see the engine without them.
        loadResources(engine, dir + QStringLiteral("/adblock-resources.json"));
    }

    {
        QMutexLocker locker(&m_loadMutex);
        m_engine = engine;
        m_loadSettled = true;
        m_ready.storeRelease(1);
    }
    m_loadDone.wakeAll();
    qInfo() << "[ADBLOCK] engine" << (engine ? "ready" : "unavailable") << "after"
            << timer.elapsed() << "ms, loaded off the GUI thread";
}

AtlanticAdblockEngine* AdBlockEngine::readyEngine()
{
    if (m_ready.loadAcquire())
        return m_engine;

    // Nothing may need the engine without a load in flight.
    startLoading();

    QElapsedTimer waited;
    waited.start();
    QMutexLocker locker(&m_loadMutex);
    while (!m_loadSettled) {
        const qint64 left = qint64(kLoadWaitMs) - waited.elapsed();
        if (left <= 0 || !m_loadDone.wait(&m_loadMutex, static_cast<unsigned long>(left)))
            break;
    }
    const bool settled = m_loadSettled;
    locker.unlock();

    qInfo() << "[ADBLOCK] waited" << waited.elapsed() << "ms for the engine"
            << (settled ? "" : "-- gave up");
    return settled ? m_engine : nullptr;
}

AtlanticAdblockEngine* AdBlockEngine::createFromCache(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "[ADBLOCK] engine cache not found:" << path;
        return nullptr;
    }
    QByteArray data = f.readAll();
    AtlanticAdblockEngine* engine = atlantic_adblock_create_from_cache(
        reinterpret_cast<const uint8_t*>(data.constData()),
        static_cast<size_t>(data.size()));
    if (!engine) {
        qWarning() << "[ADBLOCK] failed to deserialize engine cache";
        return nullptr;
    }
    qInfo() << "[ADBLOCK] engine loaded from" << (data.size() / 1024) << "KB cache";
    return engine;
}

bool AdBlockEngine::loadResources(AtlanticAdblockEngine* engine, const QString& path)
{
    if (!engine) return false;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "[ADBLOCK] scriptlet resources not found:" << path;
        return false;
    }
    QByteArray data = f.readAll();
    const bool ok = atlantic_adblock_use_resources_json(
        engine,
        reinterpret_cast<const uint8_t*>(data.constData()),
        static_cast<size_t>(data.size()));
    if (ok)
        qInfo() << "[ADBLOCK] scriptlet resources loaded from" << (data.size() / 1024) << "KB json";
    else
        qWarning() << "[ADBLOCK] scriptlet resources failed to parse:" << path;
    return ok;
}

QString AdBlockEngine::genericHides(const QUrl& url, const QByteArray& classes, const QByteArray& ids)
{
    if (!s_enabled || isAllowlistedUrl(url)) return QString();
    if (classes.isEmpty() && ids.isEmpty()) return QString();
    AtlanticAdblockEngine* engine = readyEngine();
    if (!engine) return QString();

    QByteArray urlUtf8 = url.toString().toUtf8();
    char* sels = atlantic_adblock_get_generic_hides(
        engine, urlUtf8.constData(), classes.constData(), ids.constData());
    if (!sels) return QString();
    QString result = QString::fromUtf8(sels);
    atlantic_adblock_free_string(sels);
    return result;
}

// Same relatedness rule as the WebProcess extension: equal hosts or one a
// dotted suffix of the other count as first-party.
static bool hostsRelated(const QString& a, const QString& b)
{
    if (a.isEmpty() || b.isEmpty()) return false;
    if (a.compare(b, Qt::CaseInsensitive) == 0) return true;
    if (a.length() > b.length())
        return a.endsWith(QLatin1Char('.') + b, Qt::CaseInsensitive);
    if (b.length() > a.length())
        return b.endsWith(QLatin1Char('.') + a, Qt::CaseInsensitive);
    return false;
}

bool AdBlockEngine::areHostsRelated(const QString& a, const QString& b)
{
    return hostsRelated(a, b);
}

void AdBlockEngine::setAllowlist(const QStringList& hosts)
{
    s_allowlist = hosts;
}

bool AdBlockEngine::isAllowlistedUrl(const QUrl& url)
{
    const QString host = url.host();
    if (host.isEmpty()) return false;
    for (const QString& h : s_allowlist) {
        if (hostsRelated(host, h))
            return true;
    }
    return false;
}

QByteArray AdBlockEngine::allowlistJoined()
{
    return s_allowlist.join(QLatin1Char('\n')).toUtf8();
}

bool AdBlockEngine::shouldBlockPopup(const QUrl& pageUrl, const QUrl& popupUrl)
{
    if (!s_enabled) return false;
    if (isAllowlistedUrl(pageUrl)) return false;
    if (!popupUrl.scheme().startsWith(QLatin1String("http"))) return false;
    AtlanticAdblockEngine* engine = readyEngine();
    if (!engine) return false;

    const QByteArray src = pageUrl.toString().toUtf8();
    const QByteArray req = popupUrl.toString().toUtf8();
    const int thirdParty = hostsRelated(pageUrl.host(), popupUrl.host()) ? 0 : 1;

    // "GET": every caller of this is a document navigation or a popup, which
    // the engine only ever sees as a GET. $method rules keyed to other verbs
    // correctly do not match here.
    MatchResult r = atlantic_adblock_match_network_v2(
        engine, src.constData(), req.constData(), "document", thirdParty, "GET");
    // A popup can't carry a surrogate redirect; only a plain match blocks.
    const bool block = r.matched && !r.redirect;
    atlantic_adblock_free_match_result(r);
    return block;
}

// Hosts whose cosmetic sheet is already installed on a given content manager,
// stored on the manager itself so the set dies with it.
static const char* kInstalledHostsKey = "atlantic-adblock-cosmetic-hosts";
// Scriptlet user scripts we installed, kept so resetCosmetics can remove them
// individually — the manager carries many non-adblock scripts (bridges), so
// remove_all_scripts is off limits.
static const char* kInstalledScriptsKey = "atlantic-adblock-scriptlets";

static QVector<WebKitUserScript*>* installedScripts(WebKitUserContentManager* ucm, bool create)
{
    auto* scripts = static_cast<QVector<WebKitUserScript*>*>(
        g_object_get_data(G_OBJECT(ucm), kInstalledScriptsKey));
    if (!scripts && create) {
        scripts = new QVector<WebKitUserScript*>();
        g_object_set_data_full(G_OBJECT(ucm), kInstalledScriptsKey, scripts,
                               [](gpointer p) {
                                   auto* v = static_cast<QVector<WebKitUserScript*>*>(p);
                                   for (WebKitUserScript* s : *v)
                                       webkit_user_script_unref(s);
                                   delete v;
                               });
    }
    return scripts;
}

void AdBlockEngine::installCosmetics(WebKitUserContentManager* ucm, const QUrl& url, QString* immediateScript)
{
    if (!s_enabled || !ucm || isAllowlistedUrl(url)) return;

    const QString host = url.host();
    if (host.isEmpty() || !url.scheme().startsWith(QLatin1String("http"))) return;

    auto* hosts = static_cast<QSet<QString>*>(
        g_object_get_data(G_OBJECT(ucm), kInstalledHostsKey));
    if (hosts && hosts->contains(host)) return;

    AtlanticAdblockEngine* engine = readyEngine();
    if (!engine) return;

    QByteArray urlUtf8 = url.toString().toUtf8();
    CosmeticResult cr = atlantic_adblock_get_cosmetic(engine, urlUtf8.constData());

    QString css;
    int ruleCount = 0;
    QString lazyScript;
    int lazyCount = 0;
    if (cr.hide_selectors && *cr.hide_selectors) {
        // One rule per selector: the list can contain uBO procedural selectors
        // (e.g. ":has-text(...)") that are NOT valid CSS. In a stylesheet an
        // invalid rule is dropped individually, but an invalid selector inside
        // a comma-joined group would discard the whole group — so never group.
        // (The Rust side newline-separates selectors for the same reason.)
        const QStringList sels =
            QString::fromUtf8(cr.hide_selectors).split(QLatin1Char('\n'), QString::SkipEmptyParts);

        // Single-attribute selectors ([href^="x"], a[title="y"], ...) go to the
        // lazy matcher instead of the static sheet (see kLazyAttrHidesTemplate).
        // Anything more complex (compound, :not(), flags, escapes, empty value)
        // stays static so its semantics are exactly the browser's own.
        // ATLANTIC_LAZY_ATTR_HIDES=0 disables the split; below kLazyMinRules the
        // sheet is small enough that the split buys nothing.
        static const QRegularExpression attrRe(QStringLiteral(
            "^(?:([a-zA-Z][a-zA-Z0-9-]*))?\\[\\s*([a-zA-Z_][\\w-]*)\\s*"
            "(?:([*^$]?=)\\s*(?:\"([^\"\\\\]+)\"|'([^'\\\\]+)'|([^\\s\\]\"'\\\\]+))\\s*)?\\]$"));
        static const bool lazyEnabled = qgetenv("ATLANTIC_LAZY_ATTR_HIDES") != "0";
        constexpr int kLazyMinRules = 32;

        QJsonArray lazyRules;
        QStringList staticSels;
        for (const QString& s : sels) {
            const QString t = s.trimmed();
            if (t.isEmpty()) continue;
            bool lazy = false;
            if (lazyEnabled) {
                const QRegularExpressionMatch m = attrRe.match(t);
                if (m.hasMatch()) {
                    const QString opText = m.captured(3);
                    QString value = m.captured(4);
                    if (value.isEmpty()) value = m.captured(5);
                    if (value.isEmpty()) value = m.captured(6);
                    const bool hasOp = !opText.isEmpty();
                    if (!hasOp || !value.isEmpty()) {
                        int op = 0;
                        if (opText == QLatin1String("="))       op = 1;
                        else if (opText == QLatin1String("^=")) op = 2;
                        else if (opText == QLatin1String("$=")) op = 3;
                        else if (opText == QLatin1String("*=")) op = 4;
                        lazyRules.append(QJsonArray{ m.captured(1).toLower(), m.captured(2).toLower(),
                                                     op, hasOp ? value : QString(), t });
                        lazy = true;
                    }
                }
            }
            if (!lazy) staticSels.append(t);
        }
        if (lazyRules.size() < kLazyMinRules) {
            // Too few to matter: put everything back in the static sheet.
            staticSels.clear();
            for (const QString& s : sels) {
                const QString t = s.trimmed();
                if (!t.isEmpty()) staticSels.append(t);
            }
            lazyRules = QJsonArray();
        }
        for (const QString& t : staticSels) {
            css += t + QLatin1String("{display:none!important}\n");
            ++ruleCount;
        }
        if (!lazyRules.isEmpty() && qgetenv("ATLANTIC_NO_COSMETIC_SHEET") != "1") {
            lazyCount = lazyRules.size();
            lazyScript = QString::fromUtf8(WPEUserScripts::kLazyAttrHidesTemplate);
            lazyScript.replace(QStringLiteral("/*RULES*/[]"),
                               QString::fromUtf8(QJsonDocument(lazyRules).toJson(QJsonDocument::Compact)));
        }
    }
    // generated_css last: it is untrusted list content and a stray brace in it
    // must not be able to swallow the hide rules above.
    if (cr.generated_css && *cr.generated_css)
        css += QString::fromUtf8(cr.generated_css) + QLatin1Char('\n');

    const QString scriptlets = (cr.injected_script && *cr.injected_script)
        ? QString::fromUtf8(cr.injected_script) : QString();

    atlantic_adblock_free_cosmetic(cr);

    // Scope to this exact host (each subdomain gets its own sheet on first
    // visit), all frames, user level so !important beats page styles.
    const QByteArray hostUtf8 = host.toUtf8();
    const QByteArray httpPat  = "http://"  + hostUtf8 + "/*";
    const QByteArray httpsPat = "https://" + hostUtf8 + "/*";
    const char* allowList[] = { httpPat.constData(), httpsPat.constData(), nullptr };

    // Diagnostic: ATLANTIC_NO_COSMETIC_SHEET=1 skips the hide-rule sheet (scriptlets
    // below still install) so the style-resolution cost of the sheet can be A/B'd.
    if (qgetenv("ATLANTIC_NO_COSMETIC_SHEET") == "1")
        css.clear();

    if (!css.isEmpty()) {
        WebKitUserStyleSheet* sheet = webkit_user_style_sheet_new(
            css.toUtf8().constData(),
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_STYLE_LEVEL_USER,
            allowList, nullptr);
        webkit_user_content_manager_add_style_sheet(ucm, sheet);
        webkit_user_style_sheet_unref(sheet);
        qInfo() << "[ADBLOCK] cosmetic sheet installed for" << host << "-" << ruleCount << "rules"
                << "+" << lazyCount << "lazy attribute rules";
    }

    // Lazy attribute-hide matcher: document-start script for every later load of
    // this host, plus (via immediateScript) the caller runs it in the document
    // that is loading right now.
    if (!lazyScript.isEmpty()) {
        WebKitUserScript* lazy = webkit_user_script_new(
            lazyScript.toUtf8().constData(),
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
            allowList, nullptr);
        webkit_user_content_manager_add_script(ucm, lazy);
        installedScripts(ucm, true)->append(lazy);
        if (immediateScript)
            *immediateScript = lazyScript;
        qInfo() << "[ADBLOCK] lazy attribute hides installed for" << host << "-" << lazyCount << "rules";
    }

    // ##+js(...) scriptlets as a document-start user script (they must run
    // before page scripts to be effective — the old post-load injection was a
    // no-op against anti-adblock). Installed at load-committed: the very first
    // navigation to a host may race document creation, but every later one
    // gets it at true document start.
    if (!scriptlets.isEmpty()) {
        WebKitUserScript* script = webkit_user_script_new(
            scriptlets.toUtf8().constData(),
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
            allowList, nullptr);
        webkit_user_content_manager_add_script(ucm, script);
        installedScripts(ucm, true)->append(script); // keep the ref for removal
        qInfo() << "[ADBLOCK] scriptlets installed for" << host;
    }

    if (!hosts) {
        hosts = new QSet<QString>();
        g_object_set_data_full(G_OBJECT(ucm), kInstalledHostsKey, hosts,
                               [](gpointer p) { delete static_cast<QSet<QString>*>(p); });
    }
    hosts->insert(host); // also cache "no cosmetics" hosts
}

void AdBlockEngine::resetCosmetics(WebKitUserContentManager* ucm)
{
    if (!ucm) return;
    // Cosmetic sheets are the only user style sheets we install; scriptlets
    // must be removed one by one (the manager carries other user scripts).
    webkit_user_content_manager_remove_all_style_sheets(ucm);
    if (auto* scripts = installedScripts(ucm, false)) {
        for (WebKitUserScript* s : *scripts)
            webkit_user_content_manager_remove_script(ucm, s);
    }
    g_object_set_data(G_OBJECT(ucm), kInstalledScriptsKey, nullptr);
    g_object_set_data(G_OBJECT(ucm), kInstalledHostsKey, nullptr);
}

bool AdBlockEngine::isEnabled()
{
    return s_enabled;
}

void AdBlockEngine::setEnabled(bool enabled)
{
    s_enabled = enabled;
}
