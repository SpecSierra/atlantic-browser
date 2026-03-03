/****************************************************************************
**
** Copyright (c) 2013 - 2018 Jolla Ltd.
** Copyright (c) 2020 Open Mobile Platform LLC.
** Contact: Vesa-Matti Hartikainen <vesa-matti.hartikainen@jollamobile.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// QtCore
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QProcess>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QStringList>
#include <QtCore/QtMath>
#include <QtCore/QVariant>
#include <QtCore/QVariantMap>

// QtGui
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>

#include <MDConfItem>


#include <math.h>
#include "declarativewebutils.h"
#include "browserpaths.h"

static const auto defaultUserAgentUpdateUrl = QStringLiteral("https://browser.sailfishos.org/gecko/91.0/ua-update.json");

static DeclarativeWebUtils *gSingleton = 0;

static bool fileExists(QString fileName)
{
    QFile file(fileName);
    return file.exists();
}

DeclarativeWebUtils::DeclarativeWebUtils()
    : QObject()
    , m_homePage(new MDConfItem("/apps/atlantic-browser/settings/home_page", this))
{
    QString path = BrowserPaths::dataLocation() + QStringLiteral("/.firstUseDone");
    m_firstUseDone = fileExists(path);

    connect(m_homePage, &MDConfItem::valueChanged,
            this, &DeclarativeWebUtils::homePageChanged);
}

DeclarativeWebUtils::~DeclarativeWebUtils()
{
    gSingleton = 0;
}

void DeclarativeWebUtils::handleDumpMemoryInfoRequest(const QString &fileName)
{
    if (qApp->arguments().contains("-debugMode")) {
        emit dumpMemoryInfo(fileName);
    }
}

void DeclarativeWebUtils::openUrl(const QString &url)
{

    QFileInfo fileInfo(url);
    QUrl targetUrl(url);

    if (!url.isEmpty() && targetUrl.scheme().isEmpty()) {
        QUrl tmpUrl;
        if (fileInfo.isAbsolute()) {
            tmpUrl = QUrl::fromLocalFile(url);
        } else {
            QUrl baseUrl = QUrl::fromLocalFile(QDir::currentPath() + QDir::separator());
            tmpUrl = baseUrl.resolved(url);
        }

        if (QFileInfo::exists(tmpUrl.path())) {
            targetUrl = tmpUrl;
        }
    }

    QString tmpUrl = targetUrl.toEncoded();
    emit openUrlRequested(tmpUrl);
}

void DeclarativeWebUtils::openSettings()
{
    emit openSettingsRequested();
}

void DeclarativeWebUtils::updateWebEngineSettings()
{
    // WPE: browser settings applied via WebKit API
}

void DeclarativeWebUtils::setFirstUseDone(bool firstUseDone)
{
    QString path = BrowserPaths::dataLocation() + QStringLiteral("/.firstUseDone");
    if (m_firstUseDone != firstUseDone) {
        m_firstUseDone = firstUseDone;
        QFile f(path);
        if (!firstUseDone) {
            f.remove();
        } else {
            f.open(QIODevice::ReadWrite | QIODevice::Truncate);
        }
        emit firstUseDoneChanged();
    }
}

qreal DeclarativeWebUtils::cssPixelRatio() const
{
    // WPE: return screen pixel ratio
    QScreen *screen = QGuiApplication::primaryScreen();
    return screen ? screen->devicePixelRatio() : 1.0;
}

bool DeclarativeWebUtils::firstUseDone() const
{
    return m_firstUseDone;
}

QString DeclarativeWebUtils::homePage() const
{
    return m_homePage->value("http://jolla.com").value<QString>();
}

DeclarativeWebUtils *DeclarativeWebUtils::instance()
{
    if (!gSingleton) {
        gSingleton = new DeclarativeWebUtils();
    }
    return gSingleton;
}

QString DeclarativeWebUtils::displayableUrl(const QString &fullUrl) const
{
    QUrl url(fullUrl);
    // Leaving only the scheme, host address, and port (if present).
    QString returnUrl = url.toDisplayString(QUrl::RemoveUserInfo
                                            | QUrl::RemovePath
                                            | QUrl::RemoveQuery
                                            | QUrl::RemoveFragment
                                            | QUrl::StripTrailingSlash);
    returnUrl.remove(0, returnUrl.lastIndexOf("/") + 1);
    if (returnUrl.indexOf("www.") == 0) {
        return returnUrl.remove(0, 4);
    } else if (returnUrl.indexOf("m.") == 0 && returnUrl.length() > 2) {
        return returnUrl.remove(0, 2);
    } else if (returnUrl.indexOf("mobile.") == 0 && returnUrl.length() > 7) {
        return returnUrl.remove(0, 7);
    }

    return !returnUrl.isEmpty() ? returnUrl : fullUrl;
}

QString DeclarativeWebUtils::pageName(const QString &fullUrl) const
{
    QUrl url(fullUrl);

    // Leaving only the path (if present).
    QString returnPageName = url.toDisplayString(QUrl::RemoveScheme
                                                 | QUrl::RemoveAuthority
                                                 | QUrl::RemoveQuery
                                                 | QUrl::StripTrailingSlash);

    return returnPageName.remove(0, returnPageName.lastIndexOf("/") + 1);
}

void DeclarativeWebUtils::handleObserve(const QString &message, const QVariant &data)
{
    const QVariantMap dataMap = data.toMap();
    if (message == "clipboard:setdata") {
        QClipboard *clipboard = QGuiApplication::clipboard();

        // check if we copied password
        if (!dataMap.value("private").toBool()) {
            clipboard->setText(dataMap.value("data").toString());
        }
    }
}

void DeclarativeWebUtils::setRenderingPreferences()
{
    // WPE: rendering configured via WPEQtView
}
