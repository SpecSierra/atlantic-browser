QMAKE_CXXFLAGS += -Wparentheses -Wfatal-errors -Wsuggest-override

isEmpty(DEFAULT_COMPONENT_PATH) {
  DEFINES += DEFAULT_COMPONENTS_PATH=\"\\\"$$[QT_INSTALL_LIBS]/mozembedlite/\\\"\"
} else {
  DEFINES += DEFAULT_COMPONENTS_PATH=\"\\\"$$DEFAULT_COMPONENT_PATH\\\"\"
}

DEFINES += DEFAULT_DESKTOP_BOOKMARK_ICON=\\\"icon-launcher-bookmark\\\"

CONFIG += c++1z
