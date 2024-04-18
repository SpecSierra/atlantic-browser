TARGET = tst_logins

CONFIG += link_pkgconfig

PKGCONFIG += sailfishwebengine

QT += quick concurrent sql gui-private

include(../test_common.pri)
include(../common/testobject.pri)
include(../../../apps/browser/bookmarks/bookmarks.pri)
include(../../../apps/browser/settings/settings.pri)
include(../../../apps/browser/browser.pri)
include(../../../apps/use_lib.pri)

SOURCES += tst_logins.cpp
