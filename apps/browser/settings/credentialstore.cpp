/****************************************************************************
**
** Copyright (c) 2026 Atlantic Browser.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "credentialstore.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QGuiApplication>

#include <sqlite3.h>

#include "browserpaths.h"

namespace {
// Ordered column list — shared by schema, insert and update so the binding
// order can never drift from the SQL text.
const char *const kColumns[] = {
    "hostname", "formSubmitURL", "httpRealm",
    "username", "password", "usernameField", "passwordField"
};
const int kColumnCount = int(sizeof(kColumns) / sizeof(kColumns[0]));

const char *const kSchema =
    "CREATE TABLE IF NOT EXISTS logins ("
    " id INTEGER PRIMARY KEY,"
    " hostname TEXT NOT NULL,"
    " formSubmitURL TEXT,"
    " httpRealm TEXT,"
    " username TEXT,"
    " password TEXT,"
    " usernameField TEXT,"
    " passwordField TEXT);";
}

CredentialStore *CredentialStore::instance()
{
    static CredentialStore self;
    return &self;
}

CredentialStore::CredentialStore()
{
    // Auto-lock whenever the browser leaves the foreground, so a stolen/unlocked
    // device doesn't expose stored passwords after the user switches away.
    if (qApp) {
        QObject::connect(qApp, &QGuiApplication::applicationStateChanged, this,
                         [this](Qt::ApplicationState state) {
                             if (state != Qt::ApplicationActive)
                                 lock();
                         });
    }
}

CredentialStore::~CredentialStore()
{
    if (m_db)
        sqlite3_close(m_db);
}

QString CredentialStore::databasePath() const
{
    const QString dir = BrowserPaths::dataLocation();
    if (dir.isNull())
        return QString();
    return QDir(dir).absoluteFilePath(QStringLiteral("logins.db"));
}

bool CredentialStore::isSetup() const
{
    const QString path = databasePath();
    return !path.isNull() && QFile::exists(path);
}

bool CredentialStore::openWithPassphrase(const QString &passphrase, bool creating)
{
    const QString path = databasePath();
    if (path.isNull()) {
        qWarning() << "[CredentialStore] no writable data location";
        return false;
    }

    if (sqlite3_open(path.toUtf8().constData(), &m_db) != SQLITE_OK) {
        qWarning() << "[CredentialStore] sqlite3_open failed:"
                   << (m_db ? sqlite3_errmsg(m_db) : "no handle");
        if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
        return false;
    }

    // Apply the passphrase before any other statement. Under SQLCipher this
    // runs PBKDF2 with the DB's stored salt; under stock libsqlite3 it is an
    // ignored no-op. Single quotes are doubled to keep the SQL literal valid.
    QString escaped = passphrase;
    escaped.replace(QLatin1Char('\''), QLatin1String("''"));
    const QString keyPragma = QStringLiteral("PRAGMA key = '%1';").arg(escaped);
    sqlite3_exec(m_db, keyPragma.toUtf8().constData(), nullptr, nullptr, nullptr);

    if (!creating) {
        // Verify the key: reading the schema of a SQLCipher DB with the wrong
        // passphrase fails with SQLITE_NOTADB. (Always succeeds on plaintext
        // libsqlite3 — real verification requires the sqlcipher build.)
        if (sqlite3_exec(m_db, "SELECT count(*) FROM sqlite_master;",
                         nullptr, nullptr, nullptr) != SQLITE_OK) {
            sqlite3_close(m_db);
            m_db = nullptr;
            return false; // wrong master password
        }
    }

    char *err = nullptr;
    if (sqlite3_exec(m_db, kSchema, nullptr, nullptr, &err) != SQLITE_OK) {
        qWarning() << "[CredentialStore] schema init failed:" << err;
        sqlite3_free(err);
        sqlite3_close(m_db);
        m_db = nullptr;
        return false;
    }

    m_unlocked = true;
    emit lockedChanged();
    return true;
}

bool CredentialStore::setup(const QString &masterPassword)
{
    if (isSetup()) {
        qWarning() << "[CredentialStore] setup() called but a vault already exists";
        return false;
    }
    if (masterPassword.isEmpty())
        return false;
    return openWithPassphrase(masterPassword, /*creating*/ true);
}

bool CredentialStore::unlock(const QString &masterPassword)
{
    if (isUnlocked())
        return true;
    if (!isSetup())
        return false;
    return openWithPassphrase(masterPassword, /*creating*/ false);
}

void CredentialStore::lock()
{
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
    if (m_unlocked) {
        m_unlocked = false;
        emit lockedChanged();
    }
}

QList<QPair<int, QVariantMap>> CredentialStore::all()
{
    QList<QPair<int, QVariantMap>> out;
    if (!isUnlocked())
        return out;

    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "SELECT id, hostname, formSubmitURL, httpRealm, username, password,"
        " usernameField, passwordField FROM logins ORDER BY hostname COLLATE NOCASE;";
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        qWarning() << "[CredentialStore] all() prepare failed:" << sqlite3_errmsg(m_db);
        return out;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int id = sqlite3_column_int(stmt, 0);
        QVariantMap fields;
        for (int c = 0; c < kColumnCount; ++c) {
            const int col = c + 1; // column 0 is id
            if (sqlite3_column_type(stmt, col) == SQLITE_NULL) {
                fields.insert(QString::fromLatin1(kColumns[c]), QVariant());
            } else {
                fields.insert(QString::fromLatin1(kColumns[c]),
                              QString::fromUtf8(reinterpret_cast<const char *>(
                                  sqlite3_column_text(stmt, col))));
            }
        }
        out.append(qMakePair(id, fields));
    }
    sqlite3_finalize(stmt);
    return out;
}

static void bindFields(sqlite3_stmt *stmt, const QVariantMap &fields, int firstIndex)
{
    for (int c = 0; c < kColumnCount; ++c) {
        const QString key = QString::fromLatin1(kColumns[c]);
        const QVariant v = fields.value(key);
        if (!v.isValid() || v.isNull()) {
            sqlite3_bind_null(stmt, firstIndex + c);
        } else {
            const QByteArray utf8 = v.toString().toUtf8();
            sqlite3_bind_text(stmt, firstIndex + c, utf8.constData(),
                              utf8.size(), SQLITE_TRANSIENT);
        }
    }
}

int CredentialStore::insert(const QVariantMap &fields)
{
    if (!isUnlocked())
        return -1;

    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "INSERT INTO logins (hostname, formSubmitURL, httpRealm, username,"
        " password, usernameField, passwordField) VALUES (?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        qWarning() << "[CredentialStore] insert prepare failed:" << sqlite3_errmsg(m_db);
        return -1;
    }
    bindFields(stmt, fields, 1);

    int id = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE)
        id = int(sqlite3_last_insert_rowid(m_db));
    else
        qWarning() << "[CredentialStore] insert failed:" << sqlite3_errmsg(m_db);

    sqlite3_finalize(stmt);
    return id;
}

bool CredentialStore::update(int id, const QVariantMap &fields)
{
    if (!isUnlocked())
        return false;

    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "UPDATE logins SET hostname=?, formSubmitURL=?, httpRealm=?, username=?,"
        " password=?, usernameField=?, passwordField=? WHERE id=?;";
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        qWarning() << "[CredentialStore] update prepare failed:" << sqlite3_errmsg(m_db);
        return false;
    }
    bindFields(stmt, fields, 1);
    sqlite3_bind_int(stmt, kColumnCount + 1, id);

    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok)
        qWarning() << "[CredentialStore] update failed:" << sqlite3_errmsg(m_db);
    sqlite3_finalize(stmt);
    return ok;
}

bool CredentialStore::remove(int id)
{
    if (!isUnlocked())
        return false;

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "DELETE FROM logins WHERE id=?;", -1, &stmt, nullptr) != SQLITE_OK) {
        qWarning() << "[CredentialStore] remove prepare failed:" << sqlite3_errmsg(m_db);
        return false;
    }
    sqlite3_bind_int(stmt, 1, id);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok)
        qWarning() << "[CredentialStore] remove failed:" << sqlite3_errmsg(m_db);
    sqlite3_finalize(stmt);
    return ok;
}
