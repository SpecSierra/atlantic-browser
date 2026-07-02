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
    m_openSearchPathList << QString(EMBEDLITE_CONTENT_PATH);
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
            QFile xmlFile(openSearchPath + fileName);
            xmlFile.open(QIODevice::ReadOnly | QIODevice::Text);
            QXmlStreamReader xml(&xmlFile);
            QString searchEngine;

            while (!xml.atEnd()) {
                xml.readNext();
                if (xml.isStartElement() && xml.name() == "ShortName") {
                    xml.readNext();
                    if (xml.isCharacters()) {
                        searchEngine = xml.text().toString();
                    }
                }
            }

            if (!xml.hasError()) {
                configs.insert(searchEngine, openSearchPath + fileName);
            }

            xmlFile.close();
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

const QString OpenSearchConfigs::getOpenSearchConfigPath()
{
    return QDir::homePath() + USER_OPENSEARCH_PATH;
}
