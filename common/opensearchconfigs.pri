# Engines shipped by the browser package itself. (There used to be a second
# system path, $$[QT_INSTALL_LIBS]/mozembedlite/chrome/embedlite/content/, that
# held Gecko's set; that directory does not exist on a WPE install.)
DEFINES += BROWSER_OPENSEARCH_PATH=\"\\\"/usr/share/atlantic-browser/searchEngines/\\\"\"

isEmpty(USER_OPENSEARCH_PATH) {
  DEFINES += USER_OPENSEARCH_PATH=\"\\\"/.local/share/org.sailfishos/browser/searchEngines/\\\"\"
} else {
  DEFINES += USER_OPENSEARCH_PATH=\"\\\"$$USER_OPENSEARCH_PATH\\\"\"
}

INCLUDEPATH += $$PWD

# C++ sources
SOURCES += \
    $$PWD/opensearchconfigs.cpp

# C++ headers
HEADERS += \
    $$PWD/opensearchconfigs.h
