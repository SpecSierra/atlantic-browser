INCLUDEPATH += $$PWD

CONFIG += link_pkgconfig
PKGCONFIG += mlite5
# CredentialStore uses the SQLite3 C API directly (Qt's QSQLITE driver links
# its own bundled SQLite and ignores PRAGMA key). It links the engine's
# SQLCipher build (libsqlcipher), a drop-in for SQLite whose sqlite3.h lives
# under include/sqlcipher and whose PRAGMA key encrypts the vault at rest.
# Wired via WPE_SFOS_PREFIX (not pkg-config) like the other engine libs,
# because the browser build sets PKG_CONFIG_SYSROOT_DIR, which would mis-prefix
# a WPE_SFOS_PREFIX .pc. Fallback to the sysroot's plaintext sqlite3 when the
# engine prefix isn't available (encryption then inert — dev/host builds only).
exists($${WPE_SFOS_PREFIX}/include/sqlcipher/sqlite3.h) {
    INCLUDEPATH += $${WPE_SFOS_PREFIX}/include/sqlcipher
    LIBS += -L$${WPE_SFOS_PREFIX}/lib -lsqlcipher
} else {
    PKGCONFIG += sqlite3
}

SOURCES += \
    $$PWD/searchenginemodel.cpp \
    $$PWD/logininfo.cpp \
    $$PWD/declarativeloginmodel.cpp \
    $$PWD/loginfiltermodel.cpp \
    $$PWD/credentialstore.cpp

HEADERS += \
    $$PWD/searchenginemodel.h \
    $$PWD/logininfo.h \
    $$PWD/declarativeloginmodel.h \
    $$PWD/loginfiltermodel.h \
    $$PWD/credentialstore.h
