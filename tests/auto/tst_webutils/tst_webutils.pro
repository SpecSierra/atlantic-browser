TARGET = tst_webutils
CONFIG += link_pkgconfig

PKGCONFIG += mlite5

include(../../../apps/use_lib.pri)
include(../mocks/webengine/webengine.pri)
include(../test_common.pri)

LIBS += -lgtest -lgmock

INCLUDEPATH += $$SRCDIR \
    $$CORESRCDIR

SOURCES += tst_webutils.cpp \
           $$CORESRCDIR/declarativewebutils.cpp

HEADERS += $$CORESRCDIR/declarativewebutils.h
