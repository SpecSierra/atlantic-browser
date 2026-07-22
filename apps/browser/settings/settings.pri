INCLUDEPATH += $$PWD

CONFIG += link_pkgconfig
PKGCONFIG += mlite5
# CredentialStore uses the SQLite3 C API directly (Qt's QSQLITE driver links
# its own bundled SQLite and ignores PRAGMA key). SQLCipher is a drop-in whose
# PRAGMA key encrypts the vault at rest; its sqlite3.h lives under
# include/sqlcipher.
#
# We must NOT link libsqlcipher: it exports the same sqlite3_* symbols as the
# libsqlite3 that Gecko (mozStorage) and qtcontacts/qsqlite already load into
# this process. A second copy in the global symbol scope interposes theirs and
# frees their allocations with the wrong allocator -> heap corruption -> a
# startup SIGSEGV in the cookie-DB thread, before the UI ever paints. Instead
# credentialstore.cpp dlopen()s libsqlcipher into a private
# RTLD_LOCAL|RTLD_DEEPBIND namespace at runtime, so the two SQLite copies never
# collide. Here we only need SQLCipher's header (compile time) plus libdl; the
# device still ships the runtime via the `sqlcipher` package (browser RPM
# Requires).
LIBS += -ldl
QMAKE_CXXFLAGS += $$system(pkg-config --cflags-only-I sqlcipher)

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
