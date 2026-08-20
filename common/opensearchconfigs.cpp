/****************************************************************************
**
** Copyright (c) 2015 - 2021 Jolla Ltd.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */


#include <QDir>
#include <QFile>
#include <QXmlStreamReader>
#include "opensearchconfigs.h"

OpenSearchConfigs *OpenSearchConfigs::openSearchConfigs = 0;

OpenSearchConfigs::OpenSearchConfigs(QObject *parent):QObject(parent)
{
    m_openSearchPathList << QString(BROWSER_OPENSEARCH_PATH);
    m_openSearchPathList << getOpenSearchConfigPath();
}

const StringMap OpenSearchConfigs::parseOpenSearchConfigs()
{
    StringMap configs;

    for (const QString &openSearchPath : m_openSearchPathList) {
        QDir configDir(openSearchPath);
        configDir.setSorting(QDir::Name);

        const QStringList configFiles = configDir.entryList(QStringList("*.xml"));
        for (const QString &fileName : configFiles) {
            const QString path = openSearchPath + fileName;
            const QString searchEngine = getShortName(path);
            if (!searchEngine.isEmpty()) {
                configs.insert(searchEngine, path);
            }
        }
    }
    return configs;
}

OpenSearchConfigs* OpenSearchConfigs::getInstance()
{
    if (!openSearchConfigs) {
        openSearchConfigs = new OpenSearchConfigs();
    }
    return openSearchConfigs;
}

const QStringList OpenSearchConfigs::getSearchEngineList()
{
    // Return names of search engines
    return getInstance()->parseOpenSearchConfigs().keys();
}

const StringMap OpenSearchConfigs::getAvailableOpenSearchConfigs()
{
    return getInstance()->parseOpenSearchConfigs();
}

const QString OpenSearchConfigs::getSearchUrlTemplate(const QString &engineName)
{
    const QString configFile = getAvailableOpenSearchConfigs().value(engineName);
    if (configFile.isEmpty()) {
        return QString();
    }

    QFile xmlFile(configFile);
    if (!xmlFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }

    QXmlStreamReader xml(&xmlFile);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QLatin1String("Url")
                || xml.attributes().value("type") != QLatin1String("text/html")) {
            continue;
        }

        QString url = xml.attributes().value("template").toString();
        QStringList params;
        while (!xml.atEnd() && !(xml.isEndElement() && xml.name() == QLatin1String("Url"))) {
            xml.readNext();
            // Plain <Param> only — <MozParam> entries and {moz:*} values
            // are Gecko-conditional
            if (xml.isStartElement() && xml.name() == QLatin1String("Param")) {
                const QString value = xml.attributes().value("value").toString();
                if (!value.contains(QLatin1String("{moz"))) {
                    params << xml.attributes().value("name").toString()
                              + QLatin1Char('=') + value;
                }
            }
        }
        if (!params.isEmpty()) {
            url += (url.contains(QLatin1Char('?')) ? QLatin1Char('&') : QLatin1Char('?'))
                    + params.join(QLatin1Char('&'));
        }
        if (url.contains(QLatin1String("{searchTerms}"))) {
            return url;
        }
    }
    return QString();
}

// <ShortName> is the engine's identity everywhere else in the browser: it is
// the key of the config map, the value stored in the search_engine setting and
// the label in the settings combo box. Empty means "not a usable description".
const QString OpenSearchConfigs::getShortName(const QString &configFile)
{
    QFile xmlFile(configFile);
    if (!xmlFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }

    QXmlStreamReader xml(&xmlFile);
    QString shortName;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("ShortName")) {
            xml.readNext();
            if (xml.isCharacters()) {
                shortName = xml.text().toString().trimmed();
            }
        }
    }

    return xml.hasError() ? QString() : shortName;
}

const QString OpenSearchConfigs::getOpenSearchConfigPath()
{
    return QDir::homePath() + USER_OPENSEARCH_PATH;
}
