QT += quick

# Configurable paths — override on qmake command line if needed:
#   qmake WPE_SFOS_PREFIX=/opt/wpe-sfos WPE_SOURCE_DIR=/path/to/wpewebkit-2.50.5
isEmpty(WPE_SFOS_PREFIX):  WPE_SFOS_PREFIX  = /opt/wpe-sfos
isEmpty(SFOS_SYSROOT):     SFOS_SYSROOT     = /opt/sfos-sysroot
isEmpty(WPE_SOURCE_DIR):   WPE_SOURCE_DIR   = /workspace/wpewebkit-2.50.5

INCLUDEPATH += $$PWD/core $$PWD/storage $$PWD/history $$PWD/wpe $$PWD/factories $$PWD/../common
INCLUDEPATH += $${WPE_SOURCE_DIR}/Source/WebKit/UIProcess/API/wpe/qt5
INCLUDEPATH += $${WPE_SFOS_PREFIX}/include/wpe-webkit-2.0
INCLUDEPATH += $${WPE_SFOS_PREFIX}/include/wpe-1.0
INCLUDEPATH += $${SFOS_SYSROOT}/usr/include/glib-2.0
INCLUDEPATH += $${SFOS_SYSROOT}/usr/lib64/glib-2.0/include
INCLUDEPATH += $${SFOS_SYSROOT}/usr/include/libsoup-3.0
LIBS += -L$$PWD/../build_wpe -lsailfishbrowser \
        -L$${WPE_SFOS_PREFIX}/lib/qt5/qml/org/wpewebkit/qtwpe -lqtwpe \
        -L$${SFOS_SYSROOT}/usr/lib64 -lmlite5

# Allow transitive shared library dependencies to be resolved at runtime
QMAKE_LFLAGS += -Wl,--allow-shlib-undefined
# Cross-compilation: resolve sysroot-relative linker script absolute paths
QMAKE_LFLAGS += -Wl,--sysroot=$${SFOS_SYSROOT}
