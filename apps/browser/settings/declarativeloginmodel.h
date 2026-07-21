/****************************************************************************
**
** Copyright (c) 2021 Open Mobile Platform LLC.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef LOGINMODEL_H
#define LOGINMODEL_H

#include <QAbstractListModel>
#include <QQmlParserStatus>
#include <QMap>

#include "logininfo.h"

typedef QPair<int, LoginInfo> UidLoginInfo;

class DeclarativeLoginModel : public QAbstractListModel, public QQmlParserStatus
{
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(bool populated READ populated NOTIFY populatedChanged)
    // Master-password gate (Phase 4). locked: the vault is not open this
    // session. hasVault: a vault has already been created (drives setup vs
    // unlock in the UI).
    Q_PROPERTY(bool locked READ locked NOTIFY lockedChanged)
    Q_PROPERTY(bool hasVault READ hasVault NOTIFY lockedChanged)

public:
    enum Roles {
        UidRole = Qt::UserRole,
        HostnameRole,
        UsernameRole,
        PasswordRole,
        FavIconRole
    };

    DeclarativeLoginModel(QObject *parent = nullptr);

    void classBegin() override;
    void componentComplete() override;

    bool locked() const;
    bool hasVault() const;
    // Master-password gate. createVault() makes a new vault; unlock() opens an
    // existing one (false on wrong password); lock() closes it.
    Q_INVOKABLE bool createVault(const QString &masterPassword);
    Q_INVOKABLE bool unlock(const QString &masterPassword);
    Q_INVOKABLE void lock();

    // Creates a new login. Returns the new uid, or -1 if it duplicates an
    // existing entry / the store rejected it.
    Q_INVOKABLE int add(const QString &hostname, const QString &username, const QString &password);
    Q_INVOKABLE void modify(int uid, const QString &username, const QString &password);
    Q_INVOKABLE void remove(int uid);
    Q_INVOKABLE bool canModify(int uid, const QString &username, const QString &password) const;
    // canModify requires an existing uid; add() uses this to reject duplicates.
    Q_INVOKABLE bool canAdd(const QString &hostname, const QString &username) const;

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool populated() const;

signals:
    void countChanged();
    void populatedChanged();
    void lockedChanged();

private slots:
    // Reacts to the store locking/unlocking (incl. auto-lock on background).
    void onStoreLockedChanged();

private:
    void requestLogins();
    // Returns -1 if the uid is invalid
    int indexFromUid(int uid) const;

private:
    // <uid, index>
    QMap<int, int> m_index;
    // <uid, data>
    QList<UidLoginInfo> m_logins;
    int m_nextUid;
    bool m_populated;
};

#endif // LOGINMODEL_H
