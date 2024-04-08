INCLUDEPATH += $$PWD

CONFIG += link_pkgconfig
PKGCONFIG += mlite5 sailfishwebengine

# C++ sources
SOURCES += \
    $$PWD/searchenginemodel.cpp \
    $$PWD/logininfo.cpp \
    $$PWD/declarativeloginmodel.cpp \
    $$PWD/loginfiltermodel.cpp

# C++ headers
HEADERS += \
    $$PWD/searchenginemodel.h \
    $$PWD/logininfo.h \
    $$PWD/declarativeloginmodel.h \
    $$PWD/loginfiltermodel.h
