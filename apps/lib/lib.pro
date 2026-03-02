TEMPLATE = lib
TARGET = sailfishbrowser

QT += qml quick gui gui-private dbus concurrent sql

target.path = $$[QT_INSTALL_LIBS]

include(../../defaults.pri)
include(../../common/browserapp.pri)
include(../../common/opensearchconfigs.pri)
include(../core/core.pri)
include(../history/history.pri)
include(../wpe/wpe.pri)
include(../factories/factories.pri)

INSTALLS += target
