isEmpty(EMBEDLITE_CONTENT_PATH) {
  DEFINES += EMBEDLITE_CONTENT_PATH=\"\\\"$$[QT_INSTALL_LIBS]/mozembedlite/chrome/embedlite/content/\\\"\"
} else {
  DEFINES += EMBEDLITE_CONTENT_PATH=\"\\\"$$EMBEDLITE_CONTENT_PATH\\\"\"
}

# Engines shipped by the browser package itself (in addition to the
# mozembedlite system set), e.g. Brave
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
