QT += qml quick gui dbus concurrent
# The name of your app
TARGET = atlantic-browser

CONFIG += link_pkgconfig

TARGETPATH = /usr/bin
target.path = $$TARGETPATH

INSTALLS += target

DEPLOYMENT_PATH = /usr/share/$$TARGET
DEFINES += DEPLOYMENT_PATH=\"\\\"\"$${DEPLOYMENT_PATH}/\"\\\"\"

!isEmpty(SFOS_SYSROOT) {
    QMAKE_LIBDIR += $${SFOS_SYSROOT}/usr/lib64
    QMAKE_LFLAGS += -Wl,-rpath-link,$${SFOS_SYSROOT}/usr/lib64
}


# Translations
TS_PATH = $$PWD
# Shared translations in browser.pro should be skipped from other subprojects
# to avoid duplicated ids
TS_PATH += $$PWD/../shared
TS_FILE = $$OUT_PWD/atlantic-browser.ts
EE_QM = $$OUT_PWD/atlantic-browser_eng_en.qm
include(../../translations/translations.pri)

include(../../defaults.pri)
include(../shared/shared.pri)

# QML files and folders of browser
qml.path = $$DEPLOYMENT_PATH
qml.files = qml/*.qml qml/pages qml/cover
INSTALLS += qml

# Search engines shipped by the browser (beyond the mozembedlite system set)
searchengines.path = $$DEPLOYMENT_PATH/searchEngines
searchengines.files = ../../data/searchEngines/*.xml
INSTALLS += searchengines

# C++ sources
SOURCES += main.cpp

OTHER_FILES += qml/*.qml qml/pages/*.qml qml/pages/components/*.qml qml/cover/*.qml
