INCLUDEPATH += $$PWD

CONFIG += link_pkgconfig
PKGCONFIG += mlite5 sailfishwebengine

SOURCES += \
    $$PWD/searchenginemodel.cpp \
    $$PWD/logininfo.cpp \
    $$PWD/declarativeloginmodel.cpp \
    $$PWD/loginfiltermodel.cpp

HEADERS += \
    $$PWD/searchenginemodel.h \
    $$PWD/logininfo.h \
    $$PWD/declarativeloginmodel.h \
    $$PWD/loginfiltermodel.h
