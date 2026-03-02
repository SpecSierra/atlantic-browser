/* WPE stub — page creation handled directly by WPEWebContainer */
#include "webpagefactory.h"
#include <QQmlComponent>

void WebPageFactory::updateQmlComponent(QQmlComponent *newComponent)
{
    m_qmlComponent = newComponent;
}
