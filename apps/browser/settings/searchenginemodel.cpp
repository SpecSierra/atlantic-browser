/****************************************************************************
**
** Copyright (c) 2021 Jolla Ltd.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "searchenginemodel.h"
#include "opensearchconfigs.h"
#include "datafetcher.h"

#include <QString>
#include <QFile>
#include <cstdio>

#include <MDConfItem>

const auto SEARCH_ENGINE_CONFIG = QStringLiteral("/apps/sailfish-browser/settings/search_engine");
const auto SEARCH_ENGINES_AVAILABLE_CONFIG = QStringLiteral("/apps/sailfish-browser/settings/search_engines_available");

SearchEngineModel::SearchEngineModel(QObject *parent)
    : QAbstractListModel(parent)
{
    fprintf(stderr, "[SEM] constructor start\n");
    QString userSearchPrefix = OpenSearchConfigs::getOpenSearchConfigPath();
    QMap<QString, QString> searchConfigs = OpenSearchConfigs::getAvailableOpenSearchConfigs();
    fprintf(stderr, "[SEM] got %d search configs\n", searchConfigs.count());

    for (const QString &name : searchConfigs.keys()) {
        Status status;
        if (searchConfigs.value(name).startsWith(userSearchPrefix)) {
            status = Status::UserInstalled;
        } else {
            status = Status::System;
        }

        SearchEngine engine(QUrl(), name, status);
        m_searchEngines.append(engine);
    }

    // WPE: Skip MDConfItem for available engines — causes SIGSEGV in mlite5/dconf
    // The "available" engines are user-discoverable ones from websites; not critical.
    fprintf(stderr, "[SEM] constructor done, %d engines total\n", m_searchEngines.count());
}

SearchEngineModel::~SearchEngineModel()
{
}

int SearchEngineModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return m_searchEngines.count();
}

QVariant SearchEngineModel::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= m_searchEngines.count())
        return QVariant();

    const SearchEngine &searchEngine = m_searchEngines.at(index.row());
    switch (role) {
    case UrlRole:
        return searchEngine.url;
    case TitleRole:
        return searchEngine.title;
    case StatusRole:
        return searchEngine.status;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> SearchEngineModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[UrlRole] = "url";
    roles[TitleRole] = "title";
    roles[StatusRole] = "status";
    return roles;
}

void SearchEngineModel::classBegin()
{
}

void SearchEngineModel::componentComplete()
{
}

void SearchEngineModel::add(const QString &title, const QString &url)
{
    for (const SearchEngine& engine : m_searchEngines) {
        if (engine.title == title) {
            return;
        }
    }
    SearchEngine engine(url, title, Status::Available);
    beginInsertRows(QModelIndex(), rowCount(), rowCount());
    m_searchEngines.append(engine);
    endInsertRows();
    emit countChanged();

    // WPE: MDConfItem causes SIGSEGV — skip dconf persistence
}

void SearchEngineModel::install(const QString &title)
{
    for (int i = 0; i < m_searchEngines.count(); i++) {
        if (m_searchEngines[i].title == title && m_searchEngines[i].status == Status::Available) {
            DataFetcher *fetcher = new DataFetcher();
            fetcher->setType(DataFetcher::Type::OpenSearch);
            connect(fetcher, &DataFetcher::statusChanged, [this, fetcher, i]() {
                if (fetcher->status() == DataFetcher::Status::Ready) {
                    m_searchEngines[i].status = Status::UserInstalled;
                    emit dataChanged(index(i), index(i), QVector<int>() << StatusRole);

                    // WPE: MDConfItem causes SIGSEGV — skip dconf persistence

                    emit installed(m_searchEngines[i].title);

                    fetcher->deleteLater();
                } else if (fetcher->status() == DataFetcher::Status::Error) {
                    // TODO: error notification
                    fetcher->deleteLater();
                }
            });
            fetcher->fetch(m_searchEngines[i].url.toString());
            break;
        }
    }
}

void SearchEngineModel::remove(const QString &title)
{
    // WPE: MDConfItem causes SIGSEGV — skip dconf check for active engine
    for (int i = 0; i < m_searchEngines.count(); i++) {
        if (m_searchEngines[i].title == title && m_searchEngines[i].status != Status::System) {
            beginRemoveRows(QModelIndex(), i, i);
            if (m_searchEngines[i].status == Status::Available) {
                // WPE: skip dconf persistence
            } else {
                QFile::remove(OpenSearchConfigs::getAvailableOpenSearchConfigs().value(title));
            }
            m_searchEngines.removeAt(i);
            endRemoveRows();
            break;
        }
    }
}
