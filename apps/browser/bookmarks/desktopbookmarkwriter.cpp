/****************************************************************************
**
** Copyright (c) 2014 Jolla Ltd.
** Contact: Raine Makelainen <raine.makelainen@jolla.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <QDir>
#include <QtConcurrent>

#include "desktopbookmarkwriter.h"
#include "browserpaths.h"
#include "faviconmanager.h"
#include "datafetcher.h"

static bool dbw_testMode = false;

DesktopBookmarkWriter::DesktopBookmarkWriter(QObject *parent)
    : QObject(parent)
{
    connect(&m_writer, &QFutureWatcher<QString>::finished,
            this, &DesktopBookmarkWriter::desktopFileWritten);
}

DesktopBookmarkWriter::~DesktopBookmarkWriter()
{
    if (m_writer.isRunning()) {
        m_writer.waitForFinished();
    }
}

void DesktopBookmarkWriter::setTestModeEnabled(bool testMode)
{
    dbw_testMode = testMode;
}

bool DesktopBookmarkWriter::isTestModeEnabled()
{
    return dbw_testMode;
}

void DesktopBookmarkWriter::save(const QString &url, const QString &title, const QString &icon)
{
    QString effectiveIcon = icon;
    if (url.trimmed().isEmpty() || title.trimmed().isEmpty()) {
        emit saved(QString());
        return;
    }

    if (icon.isEmpty()) {
        effectiveIcon = FaviconManager::defaultDesktopBookmarkIcon();
    }

    if (icon.startsWith(QStringLiteral("https://")) || icon.startsWith(QStringLiteral("http://"))) {
        DataFetcher *fetcher = new DataFetcher(this);
        connect(fetcher, &DataFetcher::statusChanged,
                this, [this, url, title, fetcher]() {
            if (fetcher->status() == DataFetcher::Error) {
                m_writer.setFuture(QtConcurrent::run(this, &DesktopBookmarkWriter::write, url, title,
                                                     FaviconManager::defaultDesktopBookmarkIcon()));
            } else if (fetcher->status() == DataFetcher::Ready) {
                m_writer.setFuture(QtConcurrent::run(this, &DesktopBookmarkWriter::write, url, title,
                                                     fetcher->data()));
            }
        });
        fetcher->fetch(icon);
    } else {
        m_writer.setFuture(QtConcurrent::run(this, &DesktopBookmarkWriter::write, url, title, effectiveIcon));
    }
}

void DesktopBookmarkWriter::desktopFileWritten()
{
    QString path = m_writer.result();
    emit saved(path);
}

QString DesktopBookmarkWriter::uniqueDesktopFileName(QString title)
{
    QString filePath;
    if (!isTestModeEnabled()) {
        filePath = BrowserPaths::applicationsLocation();
    } else {
        filePath = BrowserPaths::dataLocation();
    }
    title = title.simplified().replace(QString(" "), QString("-"));
    // The title becomes part of a file name: a '/' (common in page titles)
    // made the path point into a directory that does not exist, and glob
    // characters would have skewed the name filter below.
    for (int i = 0; i < title.size(); ++i) {
        const QChar c = title.at(i);
        if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c == QLatin1Char('*')
                || c == QLatin1Char('?') || c == QLatin1Char('[') || c == QLatin1Char(']')
                || c.unicode() < 0x20)
            title[i] = QLatin1Char('_');
    }
    title = title.left(80);

    QDir dir(filePath);
    dir.mkpath(filePath);

    dir.setNameFilters(QStringList() << QString("atlantic-browser-%2*").arg(title));
    QStringList similarlyNamedFiles = dir.entryList();
    int count = similarlyNamedFiles.count();

    QString fileName = QString(desktopFilePattern()).arg(title).arg(count);
    while (similarlyNamedFiles.contains(fileName)) {
        ++count;
        fileName = QString(desktopFilePattern()).arg(title).arg(count);
    }

    return filePath + '/' + fileName;
}

QString DesktopBookmarkWriter::write(const QString &url, const QString &title, const QString &icon)
{
    QString fileName = uniqueDesktopFileName(title);
    // Values are single-line keys: a newline would let the title or URL inject
    // further desktop-entry keys (Exec=...).
    auto oneLine = [](QString v) { return v.trimmed().replace(QLatin1Char('\n'), QLatin1Char(' ')).replace(QLatin1Char('\r'), QLatin1Char(' ')); };
    QString desktopFileData = QString("[Desktop Entry]\n" \
                                      "Type=Link\n" \
                                      "Name=%1\n" \
                                      "Icon=%2\n" \
                                      "URL=%3\n" \
                                      "Comment=%4\n").arg(oneLine(title), oneLine(icon),
                                                          oneLine(url), oneLine(title));
    QFile desktopFile(fileName);
    if (desktopFile.open(QFile::WriteOnly)) {
        desktopFile.write(desktopFileData.toUtf8());
        desktopFile.flush();
        desktopFile.close();
        return fileName;
    }

    return QString();
}

QString DesktopBookmarkWriter::desktopFilePattern()
{
    return QStringLiteral("atlantic-browser-%2-%3.desktop");
}
