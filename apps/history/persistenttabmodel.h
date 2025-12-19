/****************************************************************************
**
** Copyright (c) 2015 Jolla Ltd.
** Contact: Siteshwar Vashisht <siteshwar@gmail.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef PERSISTENTTABMODEL_H
#define PERSISTENTTABMODEL_H

#include "declarativetabmodel.h"

class DeclarativeWebContainer;

class PersistentTabModel : public DeclarativeTabModel
{
    Q_OBJECT

protected:
    void createTab(const Tab &tab) override;
    void updateTitle(int tabId, const QString &url, const QString &title) override;
    void removeTab(int tabId) override;
    void updateRequestedUrl(int tabId, const QString &requestedUrl, const QString &resolvedUrl) override;
    void navigateTo(int tabId, const QString &url, const QString &title, const QString &path) override;
    void updateThumbPath(int tabId, const QString &path) override;

private slots:
    void saveActiveTab() const;
    void tabsAvailable(const QList<Tab> &tabs);

public:
    PersistentTabModel(int nextTabId, DeclarativeWebContainer *webContainer = nullptr);
    ~PersistentTabModel();
};

#endif // PERSISTENTTABMODEL_H
