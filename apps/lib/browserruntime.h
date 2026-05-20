#ifndef ATLANTIC_BROWSERRUNTIME_H
#define ATLANTIC_BROWSERRUNTIME_H

#include <QtGlobal>

class QGuiApplication;
class QQuickView;

extern "C" Q_DECL_EXPORT bool atlanticBrowserRuntimeStart(QQuickView *view,
                                                          QGuiApplication *app,
                                                          const char *dataPath);

#endif // ATLANTIC_BROWSERRUNTIME_H
