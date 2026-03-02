/*
 * Copyright (c) 2013 Jolla Ltd.
 * SPDX-License-Identifier: MPL-2.0
 */

#include "closeeventfilter.h"
#include "declarativewebutils.h"
#include "dbmanager.h"

#include <QCoreApplication>
#include <MDConfItem>

CloseEventFilter::CloseEventFilter(DownloadManager *dlMgr, QObject *parent)
    : QObject(parent),
      m_downloadManager(dlMgr),
      m_closing(false)
{
    connect(&m_shutdownWatchdog, &QTimer::timeout,
            this, &CloseEventFilter::onWatchdogTimeout);
    connect(m_downloadManager, &DownloadManager::allTransfersCompleted,
            this, &CloseEventFilter::allTransfersCompleted);
}

void CloseEventFilter::applicationClosingStarted()
{
    if (!m_downloadManager->existActiveTransfers()) {
        closeApplication();
    } else {
        m_closing = true;
    }
}

void CloseEventFilter::closeApplication()
{
    if (m_downloadManager->existActiveTransfers()) {
        m_closing = true;
        return;
    }

    MDConfItem closeAllTabsConf("/apps/sailfish-browser/settings/close_all_tabs");
    if (closeAllTabsConf.value(false).toBool()) {
        DBManager::instance()->removeAllTabs();
    }

    qApp->exit();
}

void CloseEventFilter::onContextDestroyed()
{
    qApp->exit();
}

void CloseEventFilter::onWatchdogTimeout()
{
    qApp->exit();
}

void CloseEventFilter::cancelCloseApplication()
{
    m_closing = false;
    m_shutdownWatchdog.stop();
}

void CloseEventFilter::allTransfersCompleted()
{
    if (m_closing) {
        closeApplication();
    }
}
