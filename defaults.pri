QMAKE_CXXFLAGS += -Wparentheses -Wfatal-errors -Wsuggest-override

isEmpty(DEFAULT_COMPONENT_PATH) {
  DEFINES += DEFAULT_COMPONENTS_PATH=\"\\\"$$[QT_INSTALL_LIBS]/mozembedlite/\\\"\"
} else {
  DEFINES += DEFAULT_COMPONENTS_PATH=\"\\\"$$DEFAULT_COMPONENT_PATH\\\"\"
}

CONFIG += c++1z

# The phone's library directory: lib64 on aarch64, lib on armv7hl. The engine's
# build-ui.sh passes SFOS_LIBDIR; WPERuntimePaths.h builds its paths from it.
isEmpty(SFOS_LIBDIR): SFOS_LIBDIR = lib64
DEFINES += ATLANTIC_LIBDIR=\"\\\"/usr/$$SFOS_LIBDIR\\\"\"
