/*
 * Copyright (c) 2024 Jolla Ltd.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef WPEWEBPAGE_H
#define WPEWEBPAGE_H

#include <QHash>
#include <QSharedPointer>
#include <QTimer>
#include <QVariant>
#include <qqml.h>

#include "WPEQtView.h"

class QQuickItemGrabResult;
class QInputMethodEvent;
class WPEQtViewLoadRequest;

// Lightweight security info exposed to QML (mirrors QMozSecurity interface)
class WPESecurityInfo : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool validState READ validState NOTIFY changed)
    Q_PROPERTY(bool allGood READ allGood NOTIFY changed)
    Q_PROPERTY(QString subjectDisplayName READ subjectDisplayName NOTIFY changed)
    Q_PROPERTY(QString issuerDisplayName READ issuerDisplayName NOTIFY changed)
    Q_PROPERTY(QString cipherName READ cipherName NOTIFY changed)
    Q_PROPERTY(bool certIsNull READ certIsNull NOTIFY changed)
    Q_PROPERTY(QString notBefore READ notBefore NOTIFY changed)
    Q_PROPERTY(QString notAfter READ notAfter NOTIFY changed)
    Q_PROPERTY(QString certificatePem READ certificatePem NOTIFY changed)
    Q_PROPERTY(QString issuerOrganization READ issuerOrganization NOTIFY changed)
    Q_PROPERTY(QString issuerCommonName READ issuerCommonName NOTIFY changed)
    Q_PROPERTY(QString subjectOrganization READ subjectOrganization NOTIFY changed)
    Q_PROPERTY(QString errorDescription READ errorDescription NOTIFY changed)
public:
    explicit WPESecurityInfo(QObject *parent = nullptr) : QObject(parent) {}
    bool validState() const { return m_valid; }
    bool allGood() const { return m_valid && !m_hasErrors; }
    QString subjectDisplayName() const { return m_subject; }
    QString issuerDisplayName() const { return m_issuer; }
    QString cipherName() const { return m_cipher; }
    bool certIsNull() const { return !m_valid; }
    QString notBefore() const { return m_notBefore; }
    QString notAfter() const { return m_notAfter; }
    QString certificatePem() const { return m_pem; }
    QString issuerOrganization() const { return m_issuerOrg; }
    QString issuerCommonName() const { return m_issuerCN; }
    QString subjectOrganization() const { return m_subjectOrg; }
    QString errorDescription() const { return m_errorDesc; }

    void update(bool valid, bool hasErrors, const QString &subject, const QString &issuer, const QString &cipher,
                const QString &notBefore, const QString &notAfter, const QString &pem,
                const QString &issuerOrg, const QString &issuerCN, const QString &subjectOrg,
                const QString &errorDesc) {
        m_valid = valid; m_hasErrors = hasErrors; m_subject = subject; m_issuer = issuer; m_cipher = cipher;
        m_notBefore = notBefore; m_notAfter = notAfter; m_pem = pem;
        m_issuerOrg = issuerOrg; m_issuerCN = issuerCN; m_subjectOrg = subjectOrg; m_errorDesc = errorDesc;
        emit changed();
    }
    void reset() { update(false, false, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}); }

signals:
    void changed();
private:
    bool m_valid = false;
    bool m_hasErrors = false;
    QString m_subject;
    QString m_issuer;
    QString m_cipher;
    QString m_notBefore;
    QString m_notAfter;
    QString m_pem;
    QString m_issuerOrg;
    QString m_issuerCN;
    QString m_subjectOrg;
    QString m_errorDesc;
};

class WPEWebPage : public WPEQtView
{
    Q_OBJECT

    Q_PROPERTY(int tabId READ tabId WRITE setTabId NOTIFY tabIdChanged)
    Q_PROPERTY(bool painted READ painted NOTIFY paintedChanged)
    Q_PROPERTY(bool domContentLoaded READ domContentLoaded NOTIFY domContentLoadedChanged)
    Q_PROPERTY(bool chrome READ chrome WRITE setChrome NOTIFY chromeChanged)
    Q_PROPERTY(bool forcedChrome READ forcedChrome WRITE setForcedChrome NOTIFY forcedChromeChanged)
    Q_PROPERTY(bool fullscreen READ fullscreen NOTIFY fullscreenChanged)
    Q_PROPERTY(bool atYBeginning READ atYBeginning NOTIFY atYBeginningChanged)
    Q_PROPERTY(bool atYEnd READ atYEnd NOTIFY atYEndChanged)
    Q_PROPERTY(QString favicon READ favicon WRITE setFavicon NOTIFY faviconChanged)
    Q_PROPERTY(qreal fullscreenHeight READ fullscreenHeight WRITE setFullscreenHeight NOTIFY fullscreenHeightChanged)
    Q_PROPERTY(qreal toolbarHeight READ toolbarHeight WRITE setToolbarHeight NOTIFY toolbarHeightChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool throttlePainting READ throttlePainting WRITE setThrottlePainting NOTIFY throttlePaintingChanged)
    Q_PROPERTY(bool chromeGestureEnabled READ chromeGestureEnabled WRITE setChromeGestureEnabled NOTIFY chromeGestureEnabledChanged)
    Q_PROPERTY(qreal chromeGestureThreshold READ chromeGestureThreshold WRITE setChromeGestureThreshold NOTIFY chromeGestureThresholdChanged)
    Q_PROPERTY(bool fixedToolbar READ fixedToolbar WRITE setFixedToolbar NOTIFY fixedToolbarChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY loadedChanged)
    Q_PROPERTY(bool moving READ moving NOTIFY movingChanged)
    Q_PROPERTY(QVariant resurrectedContentRect READ resurrectedContentRect WRITE setResurrectedContentRect NOTIFY resurrectedContentRectChanged)
    Q_PROPERTY(bool desktopMode READ desktopMode WRITE setDesktopMode NOTIFY desktopModeChanged)
    Q_PROPERTY(bool textSelectionActive READ textSelectionActive NOTIFY textSelectionActiveChanged FINAL)
    Q_PROPERTY(QObject* textSelectionController READ textSelectionController CONSTANT FINAL)
    Q_PROPERTY(bool selectionVisible READ textSelectionActive NOTIFY textSelectionActiveChanged FINAL)
    Q_PROPERTY(QString text READ selectedText NOTIFY selectionTextChanged FINAL)
    Q_PROPERTY(bool isPhoneNumber READ isPhoneNumber NOTIFY selectionTextChanged FINAL)
    Q_PROPERTY(QString searchUri READ searchUri NOTIFY selectionTextChanged FINAL)
    Q_PROPERTY(qreal selectionStartX READ selectionStartX NOTIFY selectionHandlesUpdated FINAL)
    Q_PROPERTY(qreal selectionStartY READ selectionStartY NOTIFY selectionHandlesUpdated FINAL)
    Q_PROPERTY(qreal selectionEndX READ selectionEndX NOTIFY selectionHandlesUpdated FINAL)
    Q_PROPERTY(qreal selectionEndY READ selectionEndY NOTIFY selectionHandlesUpdated FINAL)

    // Security / TLS certificate info
    Q_PROPERTY(QObject* security READ security NOTIFY securityChanged FINAL)

    // Find-in-page
    Q_PROPERTY(bool findInPageHasResult READ findInPageHasResult NOTIFY findInPageHasResultChanged FINAL)

public:
    explicit WPEWebPage(QQuickItem *parent = nullptr);
    ~WPEWebPage() override;

    int tabId() const;
    void setTabId(int tabId);

    bool painted() const;
    bool domContentLoaded() const;

    bool chrome() const;
    void setChrome(bool chrome);

    bool forcedChrome() const;
    void setForcedChrome(bool forcedChrome);

    bool fullscreen() const;
    bool atYBeginning() const;
    bool atYEnd() const;

    QString favicon() const;
    void setFavicon(const QString &favicon);

    qreal fullscreenHeight() const;
    void setFullscreenHeight(qreal height);

    qreal toolbarHeight() const;
    void setToolbarHeight(qreal height);

    bool active() const;
    void setActive(bool active);

    bool throttlePainting() const;
    void setThrottlePainting(bool throttle);

    bool chromeGestureEnabled() const;
    void setChromeGestureEnabled(bool enabled);

    qreal chromeGestureThreshold() const;
    void setChromeGestureThreshold(qreal threshold);

    bool fixedToolbar() const;
    void setFixedToolbar(bool fixed);

    bool loaded() const;
    bool moving() const;

    QVariant resurrectedContentRect() const;
    void setResurrectedContentRect(const QVariant &rect);

    bool dragging() const;
    bool desktopMode() const;
    void setDesktopMode(bool desktop);
    void applyInitialDeviceScale(qreal scale);

    Q_INVOKABLE void loadTab(const QString &url, bool force = false);
    Q_INVOKABLE void grabToFile(const QSize &size);
    Q_INVOKABLE void grabThumbnail(const QSize &size);
    Q_INVOKABLE void forceChrome(bool forced);
    Q_INVOKABLE void suspendView();
    Q_INVOKABLE void resumeView();
    Q_INVOKABLE void sendAsyncMessage(const QString &name, const QVariant &data);
    Q_INVOKABLE void addMessageListener(const QString &name);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void moveSelectionStart(qreal cssX, qreal cssY);
    Q_INVOKABLE void moveSelectionEnd(qreal cssX, qreal cssY);
    Q_INVOKABLE void copyToClipboard();

    // Find-in-page
    Q_INVOKABLE void findText(const QString &text, bool backwards = false);
    Q_INVOKABLE void findFinish();
    bool findInPageHasResult() const { return m_findInPageHasResult; }
    void setFindInPageHasResult(bool has);

    // Security
    QObject* security() const { return m_security; }

    // Save page as PDF
    Q_INVOKABLE void savePageAsPDF(const QString &filePath);

    bool textSelectionActive() const;
    QObject* textSelectionController();
    QString selectedText() const;
    bool isPhoneNumber() const;
    QString searchUri() const;
    qreal selectionStartX() const { return m_selectionStartX; }
    qreal selectionStartY() const { return m_selectionStartY; }
    qreal selectionEndX() const { return m_selectionEndX; }
    qreal selectionEndY() const { return m_selectionEndY; }

signals:
    void tabIdChanged();
    void paintedChanged();
    void domContentLoadedChanged();
    void chromeChanged();
    void forcedChromeChanged();
    void fullscreenChanged();
    void atYBeginningChanged();
    void atYEndChanged();
    void faviconChanged();
    void fullscreenHeightChanged();
    void toolbarHeightChanged();
    void activeChanged();
    void throttlePaintingChanged();
    void chromeGestureEnabledChanged();
    void chromeGestureThresholdChanged();
    void fixedToolbarChanged();
    void loadedChanged();
    void movingChanged();
    void resurrectedContentRectChanged();
    void draggingChanged();
    void desktopModeChanged();

    void textSelectionActiveChanged();
    void selectionTextChanged();
    void selectionHandlesUpdated();

    void securityChanged();
    void findInPageHasResultChanged();
    void pdfSaved(const QString &filePath, bool success);

    void recvAsyncMessage(const QString &message, const QVariant &data);
    void recvSyncMessage(const QString &message, const QVariant &data);
    void fileGrabWritten(const QString &fileName);
    void thumbnailResult(const QString &data);
    void afterRendering();

protected:
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void touchEvent(QTouchEvent *event) override;

private slots:
    void onLoadingChanged(WPEQtViewLoadRequest *loadRequest);
    void onFrameSwapped();
    void updateSecurityInfo();

private:
    void updateFramePumpState();
    void scheduleVirtualKeyboardSync();
    void syncVirtualKeyboardToFocusedElement();
    double currentPageZoomLevel() const;
    void setPageZoomLevel(double zoomLevel);
    void rememberDefaultZoomLevel(double zoomLevel);
    double minimumPinchZoomLevel() const;
    double maximumPinchZoomLevel() const;

    int m_tabId = 0;
    bool m_painted = false;
    bool m_domContentLoaded = false;
    bool m_chrome = true;
    bool m_forcedChrome = false;
    bool m_fullscreen = false;
    bool m_atYBeginning = true;
    bool m_atYEnd = false;
    QString m_favicon;
    qreal m_fullscreenHeight = 0.0;
    qreal m_toolbarHeight = 0.0;
    bool m_active = false;
    bool m_throttlePainting = false;
    bool m_chromeGestureEnabled = true;
    qreal m_chromeGestureThreshold = 10.0;
    bool m_fixedToolbar = false;
    bool m_loaded = false;
    bool m_desktopMode = false;
    qreal m_lastScrollY = 0.0;
    QVariant m_resurrectedContentRect;
    bool m_textSelectionActive = false;
    QString m_selectedText;
    qreal m_selectionStartX = 0.0;
    qreal m_selectionStartY = 0.0;
    qreal m_selectionEndX = 0.0;
    qreal m_selectionEndY = 0.0;

    QSharedPointer<QQuickItemGrabResult> m_grabResult;
    QSharedPointer<QQuickItemGrabResult> m_thumbnailResult;

    bool m_findInPageHasResult = false;
    bool m_findInitialized = false;
    QObject *m_security = nullptr;
    bool m_pinchZoomActive = false;
    qreal m_pinchStartDistance = 0.0;
    double m_pinchStartZoomLevel = 1.0;
    double m_defaultZoomLevel = 1.0;
    bool m_defaultZoomLevelInitialized = false;
    QTimer m_framePump;
    qreal m_lastInteractionX = -1.0;
    qreal m_lastInteractionY = -1.0;
    QHash<int, QTouchEvent::TouchPoint> m_trackedTouchPoints;
};

QML_DECLARE_TYPE(WPEWebPage)

#endif // WPEWEBPAGE_H
