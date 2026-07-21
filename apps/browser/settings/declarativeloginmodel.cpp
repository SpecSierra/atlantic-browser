/****************************************************************************
**
** Copyright (c) 2021 Open Mobile Platform LLC.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <QDebug>
#include <QPair>

#include "faviconmanager.h"

#include "credentialstore.h"
#include "declarativeloginmodel.h"

DeclarativeLoginModel::DeclarativeLoginModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_nextUid(0)
    , m_populated(false)
{
    // Backed by the encrypted CredentialStore; uid == the store's rowid.
    connect(CredentialStore::instance(), &CredentialStore::lockedChanged,
            this, &DeclarativeLoginModel::onStoreLockedChanged);
}

void DeclarativeLoginModel::classBegin()
{
}

void DeclarativeLoginModel::componentComplete()
{
    // Populate only if a previous part of the session already unlocked the
    // vault; otherwise the UI shows the setup/unlock gate.
    if (CredentialStore::instance()->isUnlocked()) {
        requestLogins();
    } else if (!m_populated) {
        m_populated = true;
        emit populatedChanged();
    }
}

bool DeclarativeLoginModel::locked() const
{
    return !CredentialStore::instance()->isUnlocked();
}

bool DeclarativeLoginModel::hasVault() const
{
    return CredentialStore::instance()->isSetup();
}

bool DeclarativeLoginModel::createVault(const QString &masterPassword)
{
    return CredentialStore::instance()->setup(masterPassword);
}

bool DeclarativeLoginModel::unlock(const QString &masterPassword)
{
    return CredentialStore::instance()->unlock(masterPassword);
}

void DeclarativeLoginModel::lock()
{
    CredentialStore::instance()->lock();
}

void DeclarativeLoginModel::onStoreLockedChanged()
{
    if (CredentialStore::instance()->isUnlocked()) {
        // Just unlocked (or a new vault was created) — load its contents.
        requestLogins();
    } else {
        // Locked: drop every credential from memory immediately.
        beginResetModel();
        m_index.clear();
        m_logins.clear();
        endResetModel();
        emit countChanged();
    }
    emit lockedChanged();
}

int DeclarativeLoginModel::indexFromUid(int uid) const
{
    int index = m_index.value(uid, -1);
    return ((index >= 0) && (index < m_logins.count())) ? index : -1;
}

int DeclarativeLoginModel::add(const QString &hostname, const QString &username, const QString &password)
{
    if (hostname.isEmpty()) {
        qWarning() << "Refusing to add a login with an empty hostname";
        return -1;
    }
    if (!canAdd(hostname, username)) {
        qWarning() << "Can't add login entry that already exists";
        return -1;
    }

    QVariantMap fields;
    fields.insert(QStringLiteral("hostname"), hostname);
    // Manually-added logins fill on the same origin: mirror hostname into
    // formSubmitURL so the Phase 2 autofill match key is already populated.
    fields.insert(QStringLiteral("formSubmitURL"), hostname);
    fields.insert(QStringLiteral("username"), username);
    fields.insert(QStringLiteral("password"), password);

    const int uid = CredentialStore::instance()->insert(fields);
    if (uid < 0)
        return -1;

    const int index = m_logins.count();
    beginInsertRows(QModelIndex(), index, index);
    m_index.insert(uid, index);
    m_logins.append(UidLoginInfo(uid, LoginInfo(fields)));
    endInsertRows();
    emit countChanged();
    return uid;
}

void DeclarativeLoginModel::modify(int uid, const QString &username, const QString &password)
{
    int index = indexFromUid(uid);
    if (index < 0) {
        qWarning() << "Invalid uid when modifying login model";
        return;
    }
    LoginInfo oldLogin = m_logins.at(index).second;

    if ((oldLogin.username() == username) && (oldLogin.password() == password)) {
        return;
    }

    if (!canModify(uid, username, password)) {
        qWarning() << "Can't modify login entry to one that already exists";
        return;
    }

    LoginInfo newLogin = oldLogin;
    newLogin.setUsername(username);
    newLogin.setPassword(password);

    if (!CredentialStore::instance()->update(uid, newLogin.toMap())) {
        qWarning() << "Failed to persist modified login" << uid;
        return;
    }

    m_logins.replace(index, UidLoginInfo(uid, newLogin));

    emit dataChanged(QAbstractListModel::index(index), QAbstractListModel::index(index), QVector<int>());
}

QVariant DeclarativeLoginModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_logins.count())
        return QVariant();

    switch (role) {
    case UidRole:
        return m_logins.at(index.row()).first;
    case HostnameRole:
        return m_logins.at(index.row()).second.hostname();
    case UsernameRole:
        return m_logins.at(index.row()).second.username();
    case PasswordRole:
        return m_logins.at(index.row()).second.password();
    case FavIconRole:
        return FaviconManager::instance()->get("logins", m_logins.at(index.row()).second.hostname());
    default:
        return QVariant();
    }
}

int DeclarativeLoginModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return m_logins.count();
}

QHash<int, QByteArray> DeclarativeLoginModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[UidRole] = "uid";
    roles[HostnameRole] = "hostname";
    roles[UsernameRole] = "username";
    roles[PasswordRole] = "password";
    roles[FavIconRole] = "favicon";
    return roles;
}

bool DeclarativeLoginModel::populated() const
{
    return m_populated;
}

void DeclarativeLoginModel::requestLogins()
{
    beginResetModel();

    m_index.clear();
    m_logins.clear();

    const auto rows = CredentialStore::instance()->all();
    for (const auto &row : rows) {
        const int uid = row.first;
        m_index.insert(uid, m_logins.count());
        m_logins.append(UidLoginInfo(uid, LoginInfo(row.second)));
    }

    endResetModel();
    emit countChanged();

    if (!m_populated) {
        m_populated = true;
        emit populatedChanged();
    }
}

void DeclarativeLoginModel::remove(int uid)
{
    int index = indexFromUid(uid);
    if (index < 0) {
        qWarning() << "Invalid uid when removing item from login model";
        return;
    }

    LoginInfo login = m_logins.at(index).second;

    if (!CredentialStore::instance()->remove(uid)) {
        qWarning() << "Failed to remove login" << uid << "from store";
        return;
    }

    beginRemoveRows(QModelIndex(), index, index);

    m_logins.removeAt(index);
    m_index.remove(uid);

    // Update the index (every item above has its position decremented)
    for (int pos = index; pos < m_logins.count(); ++pos) {
        m_index.insert(m_logins.at(pos).first, pos);
    }

    endRemoveRows();
    emit countChanged();

    // Check to see whether the list still contains this hostname
    bool containsHostname = false;
    QList<UidLoginInfo>::ConstIterator iter = m_logins.constBegin();
    while (!containsHostname && iter != m_logins.constEnd()) {
        containsHostname = ((*iter).second.hostname() == login.hostname());
        ++iter;
    }
    if (!containsHostname) {
        // This is the last of its kind, so we can remove its icon
        FaviconManager::instance()->remove("logins", login.hostname());
    }
}

bool DeclarativeLoginModel::canModify(int uid, const QString &username, const QString &password) const
{
    int index = indexFromUid(uid);
    if (index < 0) {
        qWarning() << "Invalid uid when checking login model";
        return false;
    }

    bool validModification = true;
    LoginInfo update = m_logins.at(index).second;
    update.setUsername(username);
    update.setPassword(password);

    for (int pos = 0; pos < m_logins.count() && validModification; ++pos) {
        // Intended to follow the matching criteria in LoginManagerStorage_json.modifyLogin()
        // See gecko-dev/toolkit/components/passwordmgr/storage-json.js
        if ((pos != index) && update.doLoginsMatch(m_logins.at(pos).second, true)) {
            validModification = false;
        }
    }
    return validModification;
}

bool DeclarativeLoginModel::canAdd(const QString &hostname, const QString &username) const
{
    // A new login collides if the same host already stores the same username
    // (ignoring password), matching canModify()'s duplicate rule.
    QVariantMap fields;
    fields.insert(QStringLiteral("hostname"), hostname);
    fields.insert(QStringLiteral("formSubmitURL"), hostname);
    fields.insert(QStringLiteral("username"), username);
    LoginInfo candidate(fields);

    for (int pos = 0; pos < m_logins.count(); ++pos) {
        if (candidate.doLoginsMatch(m_logins.at(pos).second, true))
            return false;
    }
    return true;
}
