QT += qml quick gui dbus concurrent
# The name of your app
TARGET = sailfish-browser

CONFIG += link_pkgconfig

TARGETPATH = /usr/bin
target.path = $$TARGETPATH

INSTALLS += target
include(../use_lib.pri)

DEPLOYMENT_PATH = /usr/share/$$TARGET
DEFINES += DEPLOYMENT_PATH=\"\\\"\"$${DEPLOYMENT_PATH}/\"\\\"\"

packagesExist(qdeclarative5-boostable) {
    message("Building with qdeclarative-boostable support")
    DEFINES += HAS_BOOSTER
    PKGCONFIG += qdeclarative5-boostable
} else {
    warning("qdeclarative5-boostable not available; startup times will be slower")
}


# Translations
TS_PATH = $$PWD
# Shared translations in browser.pro should be skipped from other subprojects
# to avoid duplicated ids
TS_PATH += $$PWD/../shared
TS_FILE = $$OUT_PWD/sailfish-browser.ts
EE_QM = $$OUT_PWD/sailfish-browser_eng_en.qm
include(../../translations/translations.pri)

include(../../defaults.pri)
include(../browser/browser.pri)
include(../shared/shared.pri)
include(settings/settings.pri)
include(bookmarks/bookmarks.pri)

# QML files and folders of browser
qml.path = $$DEPLOYMENT_PATH
qml.files = qml/*.qml qml/pages qml/cover
INSTALLS += qml

# C++ sources
SOURCES += main.cpp
