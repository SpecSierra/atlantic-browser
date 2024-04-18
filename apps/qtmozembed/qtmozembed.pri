INCLUDEPATH += $$PWD

QT += concurrent

CONFIG += link_pkgconfig
PKGCONFIG += qt5embedwidget

# C++ sources
SOURCES += \
    $$PWD/declarativewebpage.cpp \
    $$PWD/declarativewebpagecreator.cpp

# C++ headers
HEADERS += \
    $$PWD/declarativewebpage.h \
    $$PWD/declarativewebpagecreator.h
