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
#include <QUrl>

SearchEngineModel *SearchEngineModel::s_instance = nullptr;

SearchEngineModel *SearchEngineModel::instance()
{
    return s_instance;
}

SearchEngineModel::SearchEngineModel(QObject *parent)
    : QAbstractListModel(parent)
{
    s_instance = this;
    QString userSearchPrefix = OpenSearchConfigs::getOpenSearchConfigPath();
    QMap<QString, QString> searchConfigs = OpenSearchConfigs::getAvailableOpenSearchConfigs();

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

    // Engines discovered on websites but not installed are deliberately not
    // persisted: they cost one <link> scan to rediscover on the next visit,
    // and a stale list of offers from sites the user no longer visits is worse
    // than no list. (MDConfItem cannot store them here anyway -- it is a no-op
    // stub in this build; see mdconfitem.h.) Installed engines persist as the
    // XML files themselves, under getOpenSearchConfigPath().
}

SearchEngineModel::~SearchEngineModel()
{
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

int SearchEngineModel::indexOfTitle(const QString &title) const
{
    for (int i = 0; i < m_searchEngines.count(); ++i) {
        if (m_searchEngines.at(i).title == title) {
            return i;
        }
    }
    return -1;
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

// Called for every page that advertises an OpenSearch description (see the
// searchEngineBridge in WPEWebPage), so it runs often and must stay cheap and
// idempotent. Dedupes on both name and description URL: the same service is
// commonly advertised under a slightly different <link title> per page.
void SearchEngineModel::add(const QString &title, const QString &url)
{
    const QUrl descriptionUrl(url);

    // Already installed from this site? The rows rebuilt from disk at startup
    // carry no description URL and are named by <ShortName>, which need not
    // match the <link title> the page advertises, so neither check below would
    // catch it -- but the file name does, since that is how it was saved.
    if (QFile::exists(OpenSearchConfigs::getOpenSearchConfigPath()
                      + descriptionUrl.host() + QStringLiteral(".xml"))) {
        return;
    }

    for (const SearchEngine& engine : m_searchEngines) {
        if (engine.title == title || engine.url == descriptionUrl) {
            return;
        }
    }
    SearchEngine engine(url, title, Status::Available);
    beginInsertRows(QModelIndex(), rowCount(), rowCount());
    m_searchEngines.append(engine);
    endInsertRows();
    emit countChanged();
}

QString SearchEngineModel::searchUrlTemplate(const QString &title) const
{
    return OpenSearchConfigs::getSearchUrlTemplate(title);
}

// Download the description an earlier add() offered and keep it. DataFetcher
// writes it to <user config path>/<host>.xml, which is one of the directories
// OpenSearchConfigs scans, so from then on the engine is indistinguishable
// from a shipped one.
void SearchEngineModel::install(const QString &title)
{
    const int row = indexOfTitle(title);
    if (row < 0 || m_searchEngines[row].status != Status::Available) {
        return;
    }

    const QUrl descriptionUrl = m_searchEngines[row].url;
    DataFetcher *fetcher = new DataFetcher();
    fetcher->setType(DataFetcher::Type::OpenSearch);
    // Look the row up again on completion rather than capturing it: rows are
    // appended (and removed) while the download is in flight.
    connect(fetcher, &DataFetcher::statusChanged, this, [this, fetcher, title, descriptionUrl]() {
        if (fetcher->status() == DataFetcher::Status::Ready) {
            fetcher->deleteLater();
            finishInstall(title, descriptionUrl);
        } else if (fetcher->status() == DataFetcher::Status::Error) {
            fetcher->deleteLater();
            emit installFailed(title);
        }
    });
    fetcher->fetch(descriptionUrl.toString());
}

// The file is on disk now. Two things have to be reconciled before the entry
// can be used: the guessed name (taken from the page's <link title>) must give
// way to the description's own <ShortName>, because that is the key the rest of
// the browser looks engines up by; and the file has to actually be an
// OpenSearch description with an HTML search URL -- it arrived off the network
// and nothing has validated it yet. A file that fails either test is deleted
// rather than left to sit in the config directory as a broken engine.
void SearchEngineModel::finishInstall(const QString &title, const QUrl &descriptionUrl)
{
    const QString path = OpenSearchConfigs::getOpenSearchConfigPath()
            + descriptionUrl.host() + QStringLiteral(".xml");
    const QString shortName = OpenSearchConfigs::getShortName(path);

    if (shortName.isEmpty() || OpenSearchConfigs::getSearchUrlTemplate(shortName).isEmpty()) {
        QFile::remove(path);
        emit installFailed(title);
        return;
    }

    const int row = indexOfTitle(title);
    if (row < 0) {
        // The offer was removed while the download ran. The file is good, so
        // keep it and list it under its real name.
        if (indexOfTitle(shortName) < 0) {
            beginInsertRows(QModelIndex(), rowCount(), rowCount());
            m_searchEngines.append(SearchEngine(descriptionUrl, shortName, Status::UserInstalled));
            endInsertRows();
            emit countChanged();
        }
        emit installed(shortName);
        return;
    }

    // The description may name an engine that is already installed (a second
    // site advertising the same service). Drop the now-redundant offer.
    const int existing = indexOfTitle(shortName);
    if (existing >= 0 && existing != row) {
        beginRemoveRows(QModelIndex(), row, row);
        m_searchEngines.removeAt(row);
        endRemoveRows();
        emit countChanged();
        emit installed(shortName);
        return;
    }

    m_searchEngines[row].title = shortName;
    m_searchEngines[row].status = Status::UserInstalled;
    emit dataChanged(index(row), index(row), QVector<int>() << TitleRole << StatusRole);
    emit installed(shortName);
}

void SearchEngineModel::remove(const QString &title)
{
    const int row = indexOfTitle(title);
    if (row < 0 || m_searchEngines[row].status == Status::System) {
        return;
    }

    beginRemoveRows(QModelIndex(), row, row);
    if (m_searchEngines[row].status == Status::UserInstalled) {
        // An offer only lives in this list; an installed engine is a file.
        QFile::remove(OpenSearchConfigs::getAvailableOpenSearchConfigs().value(title));
    }
    m_searchEngines.removeAt(row);
    endRemoveRows();
    emit countChanged();
}
