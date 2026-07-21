/****************************************************************************
**
** Copyright (c) 2026 Atlantic Browser.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CREDENTIALSTORE_H
#define CREDENTIALSTORE_H

#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QVariantMap>

struct sqlite3;

// Encrypted, master-password-locked credential store for the password manager.
//
// Uses the SQLite3 C API directly (NOT Qt's QSQLITE driver): Qt links its own
// bundled SQLite, which silently ignores `PRAGMA key`, so encryption there
// would be a no-op. Linked against libsqlcipher the passphrase key turns the
// whole database file into an AES-256 encrypted blob, with SQLCipher running
// PBKDF2 + a per-database salt internally; linked against a stock libsqlite3
// (current sysroot) the pragma is an unknown no-op and the file is plaintext.
// The engine swap `PKGCONFIG sqlite3 -> sqlcipher` activates encryption; the
// C++ is unchanged.
//
// Phase 4: the key is the user's master password (Phase 1-3 used a transparent
// device key). The password is never stored — an existing vault is verified by
// attempting to read its schema after keying; a wrong password fails that read
// under SQLCipher. The store is a session gate: locked at startup, unlocked
// once per session, and auto-locked when the app leaves the foreground.
class CredentialStore : public QObject
{
    Q_OBJECT
public:
    static CredentialStore *instance();

    // A vault has been created (the database file exists on disk).
    bool isSetup() const;
    // Currently unlocked and usable this session.
    bool isUnlocked() const { return m_db != nullptr && m_unlocked; }

    // Create a new vault keyed by masterPassword. Fails if one already exists.
    bool setup(const QString &masterPassword);
    // Open an existing vault. Returns false on a wrong password.
    bool unlock(const QString &masterPassword);
    // Close the handle and drop the key from memory.
    void lock();

    // All data accessors return empty / -1 / false while locked.
    QList<QPair<int, QVariantMap>> all();
    int insert(const QVariantMap &fields);
    bool update(int id, const QVariantMap &fields);
    bool remove(int id);

signals:
    // Emitted whenever the unlocked state changes (explicit or auto-lock).
    void lockedChanged();

private:
    CredentialStore();
    ~CredentialStore();
    Q_DISABLE_COPY(CredentialStore)

    QString databasePath() const;
    // Opens the DB, applies the passphrase, and either creates the schema
    // (creating=true) or verifies it decrypts (creating=false).
    bool openWithPassphrase(const QString &passphrase, bool creating);

    sqlite3 *m_db = nullptr;
    bool m_unlocked = false;
};

#endif // CREDENTIALSTORE_H
