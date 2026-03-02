INCLUDEPATH += $$PWD

CONFIG += link_pkgconfig
PKGCONFIG += sailfishpolicy nemotransferengine-qt5 dsme_dbus_if mlite5

QT += quick

# C++ sources (Gecko-free)
SOURCES += \
    $$PWD/browser.cpp \
    $$PWD/closeeventfilter.cpp \
    $$PWD/datafetcher.cpp \
    $$PWD/declarativewebcontainer.cpp \
    $$PWD/downloadmanager.cpp \
    $$PWD/declarativewebutils.cpp \
    $$PWD/faviconmanager.cpp \
    $$PWD/inputregion.cpp \
    $$PWD/logging.cpp \
    $$PWD/secureaction.cpp \
    $$PWD/settingmanager.cpp

# C++ headers
HEADERS += \
    $$PWD/browser.h \
    $$PWD/browser_p.h \
    $$PWD/closeeventfilter.h \
    $$PWD/datafetcher.h \
    $$PWD/declarativewebcontainer.h \
    $$PWD/declarativewebutils.h \
    $$PWD/downloadmanager.h \
    $$PWD/downloadstatus.h \
    $$PWD/faviconmanager.h \
    $$PWD/inputregion.h \
    $$PWD/logging.h \
    $$PWD/secureaction.h \
    $$PWD/settingmanager.h \
    $$PWD/mdconfitem.h
