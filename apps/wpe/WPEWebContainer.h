/*
 * WPE WebKit engine replacement for Sailfish Browser
 * WPEWebContainer - replaces DeclarativeWebContainer with a QQuickItem-based tab manager
 * SPDX-License-Identifier: LGPL-2.1+
 */

#pragma once

#include <QQuickItem>
#include <QQmlParserStatus>
#include <QQmlComponent>
#include <QMap>
#include <QPointer>

class WPEWebPage;
class DeclarativeTabModel;
class DeclarativeHistoryModel;
class PersistentTabModel;
class PrivateTabModel;

class WPEWebContainer : public QQuickItem
{
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)

    Q_PROPERTY(QQuickItem *rotationHandler MEMBER m_rotationHandler NOTIFY rotationHandlerChanged FINAL)
    Q_PROPERTY(WPEWebPage *contentItem READ contentItem NOTIFY contentItemChanged FINAL)
    Q_PROPERTY(DeclarativeTabModel *tabModel READ tabModel NOTIFY tabModelChanged FINAL)
    Q_PROPERTY(DeclarativeTabModel *persistentTabModel READ persistentTabModel CONSTANT)
    Q_PROPERTY(DeclarativeTabModel *privateTabModel READ privateTabModel CONSTANT)
    Q_PROPERTY(bool completed READ completed NOTIFY completedChanged FINAL)
    Q_PROPERTY(bool enabled MEMBER m_browserEnabled NOTIFY enabledChanged FINAL)
    Q_PROPERTY(bool foreground READ foreground WRITE setForeground NOTIFY foregroundChanged FINAL)
    Q_PROPERTY(int maxLiveTabCount MEMBER m_maxLiveTabCount NOTIFY maxLiveTabCountChanged FINAL)
    Q_PROPERTY(bool touchBlocked MEMBER m_touchBlocked NOTIFY touchBlockedChanged FINAL)
    Q_PROPERTY(bool selectionActive READ selectionActive NOTIFY selectionActiveChanged FINAL)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged FINAL)
    Q_PROPERTY(int loadProgress READ loadProgress NOTIFY loadProgressChanged FINAL)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY canGoForwardChanged FINAL)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY canGoBackChanged FINAL)
    Q_PROPERTY(int tabId READ tabId NOTIFY tabIdChanged FINAL)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged FINAL)
    Q_PROPERTY(QString url READ url NOTIFY urlChanged FINAL)
    Q_PROPERTY(bool privateMode READ privateMode WRITE setPrivateMode NOTIFY privateModeChanged FINAL)
    Q_PROPERTY(bool activeTabRendered READ activeTabRendered NOTIFY activeTabRenderedChanged FINAL)
    Q_PROPERTY(QQmlComponent *webPageComponent READ webPageComponent WRITE setWebPageComponent NOTIFY webPageComponentChanged FINAL)
    Q_PROPERTY(QObject *chromeWindow READ chromeWindow WRITE setChromeWindow NOTIFY chromeWindowChanged FINAL)
    Q_PROPERTY(bool readyToPaint READ readyToPaint WRITE setReadyToPaint NOTIFY readyToPaintChanged FINAL)
    Q_PROPERTY(Qt::ScreenOrientation pendingWebContentOrientation READ pendingWebContentOrientation NOTIFY pendingWebContentOrientationChanged FINAL)
    Q_PROPERTY(DeclarativeHistoryModel *historyModel READ historyModel WRITE setHistoryModel NOTIFY historyModelChanged FINAL)
    Q_PROPERTY(bool hasInitialUrl READ hasInitialUrl NOTIFY hasInitialUrlChanged FINAL)
    Q_PROPERTY(int visibility READ fakeVisibility NOTIFY visibilityChanged FINAL)
    Q_PROPERTY(bool needChrome READ needChrome NOTIFY needChromeChanged FINAL)

public:
    explicit WPEWebContainer(QQuickItem *parent = nullptr);
    ~WPEWebContainer() override;

    WPEWebPage *contentItem() const;
    DeclarativeTabModel *tabModel() const;
    DeclarativeTabModel *persistentTabModel() const;
    DeclarativeTabModel *privateTabModel() const;

    bool completed() const { return m_completed; }
    bool foreground() const { return m_foreground; }
    void setForeground(bool f);
    bool loading() const;
    int loadProgress() const;
    bool canGoForward() const;
    bool canGoBack() const;
    int tabId() const;
    QString title() const;
    QString url() const;
    bool privateMode() const { return m_privateMode; }
    void setPrivateMode(bool p);
    bool activeTabRendered() const { return m_activeTabRendered; }
    bool selectionActive() const;
    QQmlComponent *webPageComponent() const { return m_webPageComponent; }
    void setWebPageComponent(QQmlComponent *c);
    QObject *chromeWindow() const { return m_chromeWindow; }
    void setChromeWindow(QObject *w);
    bool readyToPaint() const { return m_readyToPaint; }
    void setReadyToPaint(bool r);
    Qt::ScreenOrientation pendingWebContentOrientation() const { return Qt::PortraitOrientation; }
    DeclarativeHistoryModel *historyModel() const { return m_historyModel; }
    void setHistoryModel(DeclarativeHistoryModel *m);
    bool hasInitialUrl() const { return !m_initialUrl.isEmpty(); }
    int fakeVisibility() const { return isVisible() ? 5 : 0; } // 5 = FullScreen
    bool needChrome() const;

    Q_INVOKABLE void load(const QString &url, const QString &title = QString(), bool newTab = false);
    Q_INVOKABLE void reload(bool force = true);
    Q_INVOKABLE void goForward();
    Q_INVOKABLE void goBack();
    Q_INVOKABLE int activateTab(int tabId, const QString &url = QString());
    Q_INVOKABLE void closeTab(int tabId);
    Q_INVOKABLE void releaseActiveTabOwnership();
    Q_INVOKABLE void updateContentOrientation(Qt::ScreenOrientation orientation);
    Q_INVOKABLE void applyContentOrientation(Qt::ScreenOrientation orientation);
    // Ad-block toggle, applied process-wide (every live page + future
    // WebProcesses). Called from BrowserPage.qml's dconf binding — C++ can't
    // read dconf itself (MDConfItem is a no-op stub in this build).
    Q_INVOKABLE void setAdBlockEnabled(bool enabled);
    // Cookie-banner auto-reject (autoconsent) toggle, same dconf-driven shape.
    Q_INVOKABLE void setCookieBannerBlockingEnabled(bool enabled);

    // QQmlParserStatus
    void classBegin() override;
    void componentComplete() override;

Q_SIGNALS:
    void rotationHandlerChanged();
    void contentItemChanged();
    void tabModelChanged();
    void completedChanged();
    void enabledChanged();
    void foregroundChanged();
    void allowHidingChanged();
    void maxLiveTabCountChanged();
    void touchBlockedChanged();
    void selectionActiveChanged();
    void portraitChanged();
    void fullscreenModeChanged();
    void fullscreenHeightChanged();
    void imOpenedChanged();
    void toolbarHeightChanged();
    void faviconChanged();
    void loadingChanged();
    void loadProgressChanged();
    void canGoForwardChanged();
    void canGoBackChanged();
    void tabIdChanged();
    void titleChanged();
    void urlChanged();
    void thumbnailPathChanged();
    void privateModeChanged();
    void activeTabRenderedChanged();
    void webPageComponentChanged(QQmlComponent *newComponent);
    void chromeWindowChanged();
    void chromeExposed();
    void readyToPaintChanged();
    void pendingWebContentOrientationChanged();
    void webContentOrientationChanged(Qt::ScreenOrientation orientation);
    void securityChanged();
    void historyModelChanged();
    void applicationClosing();
    void hasInitialUrlChanged();
    void visibilityChanged();
    void needChromeChanged();
    // New in 'next' branch
    void touched();
    void keyPressed(int key);
    void backButtonPressed();
    void forwardButtonPressed();

private Q_SLOTS:
    void onWindowChangedForForeground(QQuickWindow *win);
    void updateForegroundFromWindow();
    void onActiveTabChanged(int activeTabId);
    void onTabAdded(int tabId);
    void onTabClosed(int tabId);
    void onPageUrlChanged();
    void onPageTitleChanged();
    void onPageLoadProgressChanged();
    void onPagePaintedChanged();

private:
    void configureSandboxPaths();
    void ensureContainerHasUsableSize();
    void trackParentSize();
    void initializeTabModels(int nextTabId);
    void restoreInitialContent();
    QSizeF preferredPageSize(const QSizeF &screenSize) const;
    qreal initialPageDeviceScaleFactor(const QSizeF &screenSize) const;
    void configurePageGeometry(WPEWebPage *page, const QSizeF &screenSize);
    WPEWebPage *getOrCreatePage(int tabId);
    void activatePage(int tabId);
    void setActiveTabRendered(bool r);
    void connectPage(WPEWebPage *page);

    WPEWebPage *m_contentItem = nullptr;
    PersistentTabModel *m_persistentTabModel = nullptr;
    PrivateTabModel *m_privateTabModel = nullptr;
    DeclarativeTabModel *m_tabModel = nullptr;
    DeclarativeHistoryModel *m_historyModel = nullptr;
    QMap<int, WPEWebPage *> m_pages;
    QPointer<QQmlComponent> m_webPageComponent;
    QPointer<QObject> m_chromeWindow;
    QPointer<QQuickItem> m_rotationHandler;
    QPointer<QQuickWindow> m_foregroundWindow;
    bool m_completed = false;
    bool m_foreground = true;
    bool m_privateMode = false;
    bool m_activeTabRendered = false;
    bool m_readyToPaint = false;
    bool m_waitingForFreshTitle = false;
    bool m_touchBlocked = false;
    bool m_browserEnabled = true;
    int m_maxLiveTabCount = 5;
    QString m_initialUrl;
};

QML_DECLARE_TYPE(WPEWebContainer)
