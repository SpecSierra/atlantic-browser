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
// would be a no-op. Under libsqlcipher the passphrase key turns the whole
// database file into an AES-256 encrypted blob, with SQLCipher running PBKDF2
// + a per-database salt internally. Under a stock libsqlite3 the pragma is an
// unknown no-op and the file is plaintext. See credentialstore.cpp for why the
// library is dlopen()ed into a private namespace rather than linked.
//
// THE STORE FAILS CLOSED. Both degraded modes used to be silent, and both are
// severe: with a plaintext libsqlite3 every password sits on disk in the clear
// AND the key check below always succeeds, so any master password "unlocks"
// the vault. So encryption is now proven at runtime with `PRAGMA
// cipher_version` (SQLCipher answers, stock SQLite returns no row) before the
// vault will open at all, and the reason for any refusal is published to the
// UI — see encryptionAvailable() and vaultUnreadable().
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

    // True when the loaded SQLite really is SQLCipher and can encrypt. False
    // means the vault is unusable by design: refusing is the whole point, as
    // the alternative is cleartext passwords behind a password prompt that
    // accepts anything. Probed once, then cached.
    static bool encryptionAvailable();

    // A vault has been created (the database file exists on disk).
    bool isSetup() const;
    // Currently unlocked and usable this session.
    bool isUnlocked() const { return m_db != nullptr && m_unlocked; }

    // Set when the vault file exists but is not an encrypted database we can
    // open — e.g. it was created by a build that silently fell back to
    // plaintext. Distinct from a wrong password, which is recoverable by
    // typing the right one; this is not, and the UI must say so rather than
    // blaming the user's password. Cleared by a successful unlock().
    bool vaultUnreadable() const { return m_vaultUnreadable; }

    // Re-evaluate vaultUnreadable() against the file on disk. Call before
    // showing the password gate so an unopenable vault is reported up front
    // rather than after the user has failed an unlock they could never pass.
    void refreshVaultState();

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
    // Emitted when vaultUnreadable() changes.
    void vaultUnreadableChanged();

private:
    CredentialStore();
    ~CredentialStore();
    Q_DISABLE_COPY(CredentialStore)

    QString databasePath() const;
    // Opens the DB, applies the passphrase, and either creates the schema
    // (creating=true) or verifies it decrypts (creating=false).
    bool openWithPassphrase(const QString &passphrase, bool creating);
    void setVaultUnreadable(bool unreadable);

    sqlite3 *m_db = nullptr;
    bool m_unlocked = false;
    bool m_vaultUnreadable = false;
};

#endif // CREDENTIALSTORE_H
