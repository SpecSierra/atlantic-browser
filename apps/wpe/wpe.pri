INCLUDEPATH += $$PWD
INCLUDEPATH += /workspace/wpewebkit-2.50.5/Source/WebKit/UIProcess/API/wpe/qt5
INCLUDEPATH += /opt/wpe-sfos/include/wpe-webkit-2.0
INCLUDEPATH += /opt/wpe-sfos/include/wpe-1.0
INCLUDEPATH += /opt/sfos-sysroot/usr/include/glib-2.0
INCLUDEPATH += /opt/sfos-sysroot/usr/lib64/glib-2.0/include
INCLUDEPATH += /opt/sfos-sysroot/usr/include/libsoup-3.0
INCLUDEPATH += /opt/sfos-sysroot/usr/include/gio-unix-2.0

LIBS += -L/workspace/wpe-sfos-artifacts/lib/qt5/qml/org/wpewebkit/qtwpe -lqtwpe
LIBS += -L/opt/wpe-sfos/lib -lWPEWebKit-2.0
LIBS += -L/opt/sfos-sysroot/usr/lib64 -lgio-2.0 -lgobject-2.0 -lglib-2.0

HEADERS += \
    $$PWD/WPEWebPage.h \
    $$PWD/WPEWebContainer.h \
    $$PWD/WPEWebPageCreator.h

SOURCES += \
    $$PWD/WPEWebPage.cpp \
    $$PWD/WPEWebContainer.cpp \
    $$PWD/WPEWebPageCreator.cpp
