INCLUDEPATH += $$PWD
exists($${WPE_SOURCE_DIR}/Source/WebKit/UIProcess/API/wpe/qt5/WPEQtView.h) {
    INCLUDEPATH += $${WPE_SOURCE_DIR}/Source/WebKit/UIProcess/API/wpe/qt5
} else:exists($${WPE_SFOS_PREFIX}/include/wpe-webkit-1.0/wpe/qt/WPEQtView.h) {
    INCLUDEPATH += $${WPE_SFOS_PREFIX}/include/wpe-webkit-1.0/wpe/qt
}
INCLUDEPATH += $${WPE_SFOS_PREFIX}/include/wpe-webkit-2.0
INCLUDEPATH += $${WPE_SFOS_PREFIX}/include/wpe-1.0
INCLUDEPATH += $${SFOS_SYSROOT}/usr/include/glib-2.0
INCLUDEPATH += $${SFOS_SYSROOT}/usr/lib64/glib-2.0/include
INCLUDEPATH += $${SFOS_SYSROOT}/usr/include/libsoup-3.0
INCLUDEPATH += $${SFOS_SYSROOT}/usr/include/gio-unix-2.0

LIBS += -L$${WPE_SFOS_PREFIX}/lib/qt5/qml/org/wpewebkit/qtwpe -lqtwpe
LIBS += -L$${WPE_SFOS_PREFIX}/lib -lWPEWebKit-2.0
LIBS += -L$${SFOS_SYSROOT}/usr/lib64 -lgio-2.0 -lgobject-2.0 -lglib-2.0
LIBS += -L$${WPE_SFOS_PREFIX}/lib -latlantic_adblock

HEADERS += \
    $$PWD/AdBlockEngine.h \
    $$PWD/WPEWebPage.h \
    $$PWD/WPEWebContainer.h \
    $$PWD/WPEWebPageCreator.h

SOURCES += \
    $$PWD/AdBlockEngine.cpp \
    $$PWD/WPEWebPage.cpp \
    $$PWD/WPEWebContainer.cpp \
    $$PWD/WPEWebPageCreator.cpp
