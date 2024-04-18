TARGET = tst_dbmanager

QT += quick concurrent sql

include(../test_common.pri)
include(../mocks/faviconmanager/faviconmanager_mock.pri)
include(../../../apps/use_lib.pri)

SOURCES += tst_dbmanager.cpp
