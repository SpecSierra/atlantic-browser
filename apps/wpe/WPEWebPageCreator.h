#pragma once
#include <QObject>
#include <QPointer>

class WPEWebPage;
class DeclarativeTabModel;

/**
 * Stub replacement for DeclarativeWebPageCreator.
 * WPEWebContainer handles all page lifecycle; this class exists only so that
 * QML code that references WebPageCreator can still compile and register.
 */
class WPEWebPageCreator : public QObject
{
    Q_OBJECT
    Q_PROPERTY(WPEWebPage *activeWebPage READ activeWebPage WRITE setActiveWebPage NOTIFY activeWebPageChanged)
    Q_PROPERTY(DeclarativeTabModel *model READ model WRITE setModel NOTIFY modelChanged)

public:
    explicit WPEWebPageCreator(QObject *parent = nullptr);
    ~WPEWebPageCreator() override;

    WPEWebPage *activeWebPage() const;
    void setActiveWebPage(WPEWebPage *page);

    DeclarativeTabModel *model() const;
    void setModel(DeclarativeTabModel *model);

signals:
    void activeWebPageChanged();
    void modelChanged();

private:
    QPointer<WPEWebPage>          m_activeWebPage;
    QPointer<DeclarativeTabModel> m_model;
};
