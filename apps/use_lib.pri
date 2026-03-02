QT += quick

INCLUDEPATH += $$PWD/core $$PWD/storage $$PWD/history $$PWD/wpe $$PWD/factories $$PWD/../common
INCLUDEPATH += /workspace/wpewebkit-2.50.5/Source/WebKit/UIProcess/API/wpe/qt5
INCLUDEPATH += /opt/wpe-sfos/include/wpe-webkit-2.0
INCLUDEPATH += /opt/wpe-sfos/include/wpe-1.0
INCLUDEPATH += /opt/sfos-sysroot/usr/include/glib-2.0
INCLUDEPATH += /opt/sfos-sysroot/usr/lib64/glib-2.0/include
INCLUDEPATH += /opt/sfos-sysroot/usr/include/libsoup-3.0
LIBS += -L$$PWD/../build_wpe -lsailfishbrowser \
        -L/workspace/wpe-sfos-artifacts/lib/qt5/qml/org/wpewebkit/qtwpe -lqtwpe \
        -L/opt/sfos-sysroot/usr/lib64 -lmlite5

# Allow transitive shared library dependencies to be resolved at runtime
QMAKE_LFLAGS += -Wl,--allow-shlib-undefined
# Cross-compilation: resolve sysroot-relative linker script absolute paths
QMAKE_LFLAGS += -Wl,--sysroot=/opt/sfos-sysroot
