#include "WPEWebPageCreator.h"
#include "WPEWebPage.h"
#include "declarativetabmodel.h"

WPEWebPageCreator::WPEWebPageCreator(QObject *parent)
    : QObject(parent)
{
}

WPEWebPageCreator::~WPEWebPageCreator() = default;

WPEWebPage *WPEWebPageCreator::activeWebPage() const
{
    return m_activeWebPage;
}

void WPEWebPageCreator::setActiveWebPage(WPEWebPage *page)
{
    if (m_activeWebPage != page) {
        m_activeWebPage = page;
        emit activeWebPageChanged();
    }
}

DeclarativeTabModel *WPEWebPageCreator::model() const
{
    return m_model;
}

void WPEWebPageCreator::setModel(DeclarativeTabModel *model)
{
    if (m_model != model) {
        m_model = model;
        emit modelChanged();
    }
}

