INCLUDEPATH += $$PWD

CONFIG += link_pkgconfig
PKGCONFIG += mlite5
# CredentialStore uses the SQLite3 C API directly (Qt's QSQLITE driver links
# its own bundled SQLite and ignores PRAGMA key). It links Jolla's stock
# SQLCipher (sqlcipher-devel in the sysroot), a drop-in for SQLite whose
# sqlite3.h lives under include/sqlcipher and whose PRAGMA key encrypts the
# vault at rest. sqlcipher.pc's paths are under /usr, so PKG_CONFIG_SYSROOT_DIR
# rewrites them correctly (unlike a WPE_SFOS_PREFIX .pc). The device provides
# the runtime via the `sqlcipher` package (Requires in the browser RPM).
PKGCONFIG += sqlcipher

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
