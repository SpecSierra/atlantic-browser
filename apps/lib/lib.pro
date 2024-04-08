TEMPLATE = lib
TARGET = sailfishbrowser

QT += qml quick gui gui-private dbus concurrent sql

CONFIG += link_pkgconfig

target.path = $$[QT_INSTALL_LIBS]

PKGCONFIG += mlite5 sailfishwebengine

include(../../defaults.pri)
include(../../common/browserapp.pri)
include(../../common/opensearchconfigs.pri)
include(../core/core.pri)
include(../history/history.pri)
include(../qtmozembed/qtmozembed.pri)
include(../factories/factories.pri)

INSTALLS += target
