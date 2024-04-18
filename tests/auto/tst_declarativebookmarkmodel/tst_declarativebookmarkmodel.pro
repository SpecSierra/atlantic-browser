TARGET = tst_declarativebookmarkmodel

QT += network concurrent

include(../test_common.pri)
include(../../../apps/browser/bookmarks/bookmarks.pri)
include(../../../apps/use_lib.pri)

# C++ sources
SOURCES += tst_declarativebookmarkmodel.cpp
