TARGET = tst_webpages

CONFIG += link_pkgconfig

PKGCONFIG += mlite5

QT += qml quick concurrent sql gui-private

include(../mocks/webengine/webengine.pri)
include(../mocks/webpagefactory/webpagefactory.pri)
include(../mocks/declarativewebpage/declarativewebpage_mock.pri)
include(../mocks/declarativewebutils/declarativewebutils_mock.pri)
include(../mocks/downloadmanager/downloadmanager_mock.pri)
include(../mocks/opensearchconfigs/opensearchconfigs_mock.pri)

include(../test_common.pri)
include(../../../common/browserapp.pri)
include(../../../apps/core/core.pri)
include(../../../apps/history/history.pri)

LIBS += -lgtest -lgmock

SOURCES += tst_webpages.cpp
