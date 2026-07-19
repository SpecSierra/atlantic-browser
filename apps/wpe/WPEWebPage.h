/*
 * Copyright (c) 2024 Jolla Ltd.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <QHash>
#include <QSharedPointer>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QElapsedTimer>
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
    Q_PROPERTY(bool mediaAudioActive READ mediaAudioActive NOTIFY mediaAudioActiveChanged FINAL)
    Q_PROPERTY(bool mediaVideoActive READ mediaVideoActive NOTIFY mediaVideoActiveChanged FINAL)
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

    // TLS certificate failure (self-signed, expired, ...) — the load is blocked;
    // QML shows a banner and may call acceptTlsCertificate() to proceed.
    Q_PROPERTY(bool tlsErrorPending READ tlsErrorPending NOTIFY tlsErrorChanged FINAL)
    Q_PROPERTY(QString tlsErrorHost READ tlsErrorHost NOTIFY tlsErrorChanged FINAL)
    Q_PROPERTY(QString tlsErrorMessage READ tlsErrorMessage NOTIFY tlsErrorChanged FINAL)

    // Permission request (geolocation, camera, microphone) — QML shows a
    // banner and resolves it via resolvePermission(). Decisions are
    // remembered per host+type for the session. permissionType is one of
    // "geolocation", "camera", "microphone", "camera+microphone".
    Q_PROPERTY(bool permissionPending READ permissionPending NOTIFY permissionChanged FINAL)
    Q_PROPERTY(QString permissionHost READ permissionHost NOTIFY permissionChanged FINAL)
    Q_PROPERTY(QString permissionType READ permissionType NOTIFY permissionChanged FINAL)

    // Find-in-page
    Q_PROPERTY(bool findInPageHasResult READ findInPageHasResult NOTIFY findInPageHasResultChanged FINAL)

    // HTML <select> dropdown state — property bindings for reliable QML observation
    Q_PROPERTY(bool selectMenuActive READ selectMenuActive NOTIFY selectMenuActiveChanged FINAL)
    Q_PROPERTY(QStringList selectMenuOptions READ selectMenuOptions NOTIFY selectMenuOptionsChanged FINAL)
    Q_PROPERTY(int selectMenuSelectedIndex READ selectMenuSelectedIndex NOTIFY selectMenuSelectedIndexChanged FINAL)
    // Image long-press state — property binding (not recvAsyncMessage) because the
    // WebKit script-message callback runs outside the QML JS context, so a plain
    // signal to onRecvAsyncMessage never fires. A NOTIFY property is observed
    // reliably via QML bindings (same approach as selectMenu* above).
    Q_PROPERTY(QString imageLongPressUrl READ imageLongPressUrl NOTIFY imageLongPressUrlChanged FINAL)
    Q_PROPERTY(bool fileChooserActive READ fileChooserActive NOTIFY fileChooserActiveChanged FINAL)
    Q_PROPERTY(QStringList fileChooserNameFilters READ fileChooserNameFilters NOTIFY fileChooserNameFiltersChanged FINAL)
    Q_PROPERTY(bool fileChooserSelectMultiple READ fileChooserSelectMultiple NOTIFY fileChooserSelectMultipleChanged FINAL)

    // HTML5 date/time/color input pickers — same JS-bridge + NOTIFY-property
    // pattern as selectMenu* above. WebKit has no native WPE picker for these,
    // so QML drives the Silica DatePicker/TimePicker/ColorPicker dialogs.
    // inputPickerType is one of date|month|week|time|datetime-local|color;
    // value/min/max/step are the raw HTML string values.
    Q_PROPERTY(bool inputPickerActive READ inputPickerActive NOTIFY inputPickerActiveChanged FINAL)
    Q_PROPERTY(QString inputPickerType READ inputPickerType NOTIFY inputPickerChanged FINAL)
    Q_PROPERTY(QString inputPickerValue READ inputPickerValue NOTIFY inputPickerChanged FINAL)
    Q_PROPERTY(QString inputPickerMin READ inputPickerMin NOTIFY inputPickerChanged FINAL)
    Q_PROPERTY(QString inputPickerMax READ inputPickerMax NOTIFY inputPickerChanged FINAL)
    Q_PROPERTY(QString inputPickerStep READ inputPickerStep NOTIFY inputPickerChanged FINAL)

    Q_PROPERTY(bool crashed READ crashed NOTIFY crashedChanged FINAL)

    // Pinch zoom visual scale — compositor-level transform during gesture; WebKit zoom committed on end
    Q_PROPERTY(qreal visualScale READ visualScale NOTIFY visualScaleChanged)
    Q_PROPERTY(qreal pinchCenterX READ pinchCenterX NOTIFY pinchCenterChanged)
    Q_PROPERTY(qreal pinchCenterY READ pinchCenterY NOTIFY pinchCenterChanged)

    Q_PROPERTY(bool adBlockEnabled READ adBlockEnabled WRITE setAdBlockEnabled NOTIFY adBlockEnabledChanged FINAL)

    Q_PROPERTY(bool cookieBannerBlockingEnabled READ cookieBannerBlockingEnabled WRITE setCookieBannerBlockingEnabled NOTIFY cookieBannerBlockingEnabledChanged FINAL)

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
    bool mediaAudioActive() const { return m_mediaAudioActive; }
    bool mediaVideoActive() const { return m_mediaVideoActive; }
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

    // App-level foreground state (window maximized + application active),
    // pushed down by WPEWebContainer. Combined with the per-tab active flag
    // to decide whether WebKit should treat the page as visible.
    void setAppForeground(bool foreground);

    // All live WPEWebPage instances. Pages get only a visual parent
    // (setParentItem), never a QObject parent, so findChildren() on the view
    // cannot discover them — the render-recovery code must use this instead.
    static const QList<WPEWebPage *> &liveInstances();

    // Apply the ad-block toggle process-wide: flips the UI-process cosmetic
    // gate and notifies every live page's WebProcess extension. Driven by the
    // dconf binding in BrowserPage.qml (via WPEWebContainer::setAdBlockEnabled),
    // so it works with any number of tabs (or none) open.
    static void applyAdBlockEnabledGlobally(bool enabled);
    static void applyAdBlockAllowlistGlobally(const QString &json);
    static void applyCookieBannerBlockingGlobally(bool enabled);
    // Per-site UA overrides (JSON object host → profile id) from the
    // site_ua_overrides dconf key, pushed via WPEWebContainer like the two
    // toggles above.
    static void applySiteUaOverridesGlobally(const QString &json);

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
    // Pick and apply the UA for a destination URL (handles per-site quirks
    // such as Google Maps); called from the navigation policy handler.
    void applyUserAgentForUrl(const QUrl &url);
    // True when the URL gets a per-site UA quirk (Maps, Cloudflare hosts).
    bool urlHasUaQuirk(const QUrl &url) const;

    // Popunder guard: after a popup is routed into this view, same-tab
    // scripted redirects to an unrelated site are blocked for a short window
    // (the reverse-popunder puts the real content in the popup and JS-redirects
    // the original tab to the ad).
    void notePopupRouted(const QString &url);
    bool popupGuardShouldBlock(const QString &url, int navigationType);
    void applyInitialDeviceScale(qreal scale);
    void setFullscreenState(bool fullscreen);
    void setNativeFullscreenRequested(bool fullscreen);
    void setDomFullscreenActive(bool fullscreen);
    void setMediaPlaybackState(bool audioActive, bool videoActive);
    void updateObservedMediaState(bool audioActive, bool videoActive, bool fullscreenActive,
                                  qreal volume, bool muted, bool volumeChangedByPage);
    Q_INVOKABLE void setMediaMuted(bool muted);

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
    Q_INVOKABLE void handleJsSelectionClear();
    Q_INVOKABLE void handleJsSelectionUpdate(const QString &text, qreal startX, qreal startY, qreal endX, qreal endY);

    // Editable focus reported from cross-origin subframes (editableFocus bridge)
    void handleSubframeEditableFocus(bool focused);
    bool subframeEditableFocused() const { return m_subframeEditableFocus; }
    void sendNativeTextViaKeys(const QString& text, int replaceBefore);

    // Find-in-page
    Q_INVOKABLE void findText(const QString &text, bool backwards = false);
    Q_INVOKABLE void findFinish();
    bool findInPageHasResult() const { return m_findInPageHasResult; }
    void setFindInPageHasResult(bool has);

    // Security
    QObject* security() const { return m_security; }

    // TLS certificate failure
    bool tlsErrorPending() const { return m_tlsErrorPending; }
    QString tlsErrorHost() const { return m_tlsErrorHost; }
    QString tlsErrorMessage() const { return m_tlsErrorMessage; }
    Q_INVOKABLE void acceptTlsCertificate();
    void handleTlsErrorLoadFailed(const QString &failingUri, void *certificate, unsigned flags);

    // Permission prompt (geolocation / camera / microphone)
    bool permissionPending() const { return m_pendingPermission != nullptr; }
    QString permissionHost() const { return m_permissionHost; }
    QString permissionType() const { return m_permissionType; }
    Q_INVOKABLE void resolvePermission(bool allow);
    void handlePermissionRequest(void *request); // WebKitPermissionRequest*, takes a ref

    // Save page as PDF

    // HTML <select> dropdown
    Q_INVOKABLE void selectMenuOption(int index);
    Q_INVOKABLE void downloadUrl(const QString &url);
    Q_INVOKABLE void closeSelectMenu();
    Q_INVOKABLE void chooseFiles(const QStringList &filePaths);
    Q_INVOKABLE void cancelFileChooser();
    void openSelectMenu(const QStringList &options, int selectedIndex);
    bool selectMenuActive() const { return m_selectMenuActive; }

    // Date/time/color input picker: QML calls resolveInputPicker(value) with the
    // HTML-formatted value string on accept, or cancelInputPicker() on dismiss.
    Q_INVOKABLE void resolveInputPicker(const QString &value);
    Q_INVOKABLE void cancelInputPicker();
    void openInputPicker(const QString &type, const QString &value,
                         const QString &min, const QString &max, const QString &step);
    bool inputPickerActive() const { return m_inputPickerActive; }
    QString inputPickerType() const { return m_inputPickerType; }
    QString inputPickerValue() const { return m_inputPickerValue; }
    QString inputPickerMin() const { return m_inputPickerMin; }
    QString inputPickerMax() const { return m_inputPickerMax; }
    QString inputPickerStep() const { return m_inputPickerStep; }

    // Image long-press
    void openImageLongPress(const QString &imageUrl);
    Q_INVOKABLE void clearImageLongPress();
    QString imageLongPressUrl() const { return m_imageLongPressUrl; }

    QStringList selectMenuOptions() const { return m_selectMenuOptions; }
    int selectMenuSelectedIndex() const { return m_selectMenuSelectedIdx; }
    bool fileChooserActive() const { return m_fileChooserActive; }
    QStringList fileChooserNameFilters() const { return m_fileChooserNameFilters; }
    bool fileChooserSelectMultiple() const { return m_fileChooserSelectMultiple; }

    // Pinch zoom visual scale
    qreal visualScale() const { return m_visualScale; }
    qreal pinchCenterX() const { return m_pinchCenterX; }
    qreal pinchCenterY() const { return m_pinchCenterY; }

    bool crashed() const { return m_crashed; }

    bool adBlockEnabled() const;
    void setAdBlockEnabled(bool enabled);

    bool cookieBannerBlockingEnabled() const;
    void setCookieBannerBlockingEnabled(bool enabled);

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
    void mediaAudioActiveChanged();
    void mediaVideoActiveChanged();
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
    // Fired on every new touch sequence (TouchBegin); forwarded by
    // WPEWebContainer::touched() for the active page so QML can track
    // user activity (fullscreen close button, toolbar auto-hide).
    void touched();

    void textSelectionActiveChanged();
    void selectionTextChanged();
    void selectionHandlesUpdated();

    void securityChanged();
    void tlsErrorChanged();
    void permissionChanged();
    void findInPageHasResultChanged();

    void recvAsyncMessage(const QString &message, const QVariant &data);
    void fileGrabWritten(const QString &fileName);
    void thumbnailResult(const QString &data);
    void afterRendering();

    void selectMenuActiveChanged();
    void selectMenuOptionsChanged();
    void selectMenuSelectedIndexChanged();
    void imageLongPressUrlChanged();
    void fileChooserActiveChanged();
    void fileChooserNameFiltersChanged();
    void fileChooserSelectMultipleChanged();
    void inputPickerActiveChanged();
    void inputPickerChanged();

    void visualScaleChanged();
    void pinchCenterChanged();
    void crashedChanged();
    void adBlockEnabledChanged();
    void cookieBannerBlockingEnabledChanged();

public:
    // Last committed main-frame URL; source URL for redirect-hop ad blocking
    // (written/read by the load-changed C callbacks).
    QUrl m_lastCommittedUrl;

protected:
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void sendNativeEnterKey();
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void hoverEnterEvent(QHoverEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void touchEvent(QTouchEvent *event) override;

private slots:
    void onLoadingChanged(WPEQtViewLoadRequest *loadRequest);
    void onFrameSwapped();
    void updateSecurityInfo();

private:
    void updateWebKitVisibility();
    qreal selectionDisplayScale() const;
    void syncEffectiveFullscreenState();
    void updateFramePumpState();
    void scheduleVirtualKeyboardSync();
    void syncVirtualKeyboardToFocusedElement();
    double currentPageZoomLevel() const;
    void setPageZoomLevel(double zoomLevel);
    void rememberDefaultZoomLevel(double zoomLevel);
    double minimumPinchZoomLevel() const;
    double maximumPinchZoomLevel() const;
    bool handleFileChooserRequest(WebKitFileChooserRequest *request);
    void clearFileChooserRequest(bool cancelRequest);

    int m_tabId = 0;
    bool m_painted = false;
    bool m_domContentLoaded = false;
    bool m_subframeEditableFocus = false;
    bool m_chrome = true;
    bool m_forcedChrome = false;
    bool m_fullscreen = false;
    bool m_nativeFullscreenRequested = false;
    bool m_domFullscreenActive = false;
    bool m_mediaAudioActive = false;
    bool m_mediaVideoActive = false;
    qreal m_mediaVolume = 1.0;
    bool m_mediaMuted = false;
    bool m_atYBeginning = true;
    bool m_atYEnd = false;
    QString m_favicon;
    qreal m_fullscreenHeight = 0.0;
    qreal m_toolbarHeight = 0.0;
    bool m_active = false;
    bool m_appForeground = true;
    bool m_throttlePainting = false;
    bool m_chromeGestureEnabled = true;
    qreal m_chromeGestureThreshold = 10.0;
    bool m_fixedToolbar = false;
    bool m_loaded = false;
    bool m_desktopMode = false;
    qint64 m_popupRoutedAtMs = 0;
    QString m_popupRoutedUrl;
    qreal m_lastScrollY = 0.0;
    QVariant m_resurrectedContentRect;
    bool m_textSelectionActive = false;
    QString m_selectedText;
    qreal m_selectionStartX = 0.0;
    qreal m_selectionStartY = 0.0;
    qreal m_selectionEndX = 0.0;
    qreal m_selectionEndY = 0.0;

    bool m_findInPageHasResult = false;
    bool m_findInitialized = false;
    QObject *m_security = nullptr;
    bool m_pinchZoomActive = false;
    qreal m_pinchStartDistance = 0.0;
    double m_pinchStartZoomLevel = 1.0;
    double m_defaultZoomLevel = 1.0;
    bool m_defaultZoomLevelInitialized = false;
    qreal m_visualScale = 1.0;
    qreal m_pinchStartVisualScale = 1.0;
    qreal m_pinchCenterX = 0.0;
    qreal m_pinchCenterY = 0.0;
    QTimer m_framePump;
    QTimer m_mediaInactiveDebounceTimer;
    QTimer m_deferredFullscreenLeaveTimer;
    QTimer m_chromeGestureDebounceTimer;
    bool m_pendingChrome = true;
    bool m_chromeGestureArmed = false;
    // UI-process "pull down to reveal chrome" gesture (touchEvent): accumulated
    // downward finger travel of the current single touch. Show-only backup for
    // the in-page scroll bridge, which starves when page JS hogs the main thread.
    qreal m_uiGestureLastY = 0.0;
    qreal m_uiGestureAccumDown = 0.0;
    bool m_uiGestureTracking = false;
    bool m_pendingFullscreenEntry = false;
    QTimer m_pendingFullscreenEntryGuard;
    QTimer m_fullscreenEnteredGuard;      // 3s guard after entering: blocks spurious false reports
    QElapsedTimer m_lastNativeFullscreenEnter;
    QElapsedTimer m_perfFrameLogWindow;
    int m_perfFramesInWindow = 0;
    qreal m_lastInteractionX = -1.0;
    qreal m_lastInteractionY = -1.0;
    QHash<int, QTouchEvent::TouchPoint> m_trackedTouchPoints;
    QString m_lastSoftKeyboardText;
    QString m_lastPreeditText;
    qint64 m_lastSoftKeyboardTextTimeMs = 0;
    qint64 m_lastSoftBackspaceTimeMs = 0;
    qint64 m_lastSoftEnterTimeMs = 0;

    bool m_selectMenuActive = false;
    QStringList m_selectMenuOptions;
    int m_selectMenuSelectedIdx = 0;
    QString m_imageLongPressUrl;
    WebKitFileChooserRequest *m_fileChooserRequest = nullptr;
    bool m_fileChooserActive = false;
    QStringList m_fileChooserNameFilters;
    bool m_fileChooserSelectMultiple = false;

    bool m_inputPickerActive = false;
    QString m_inputPickerType;
    QString m_inputPickerValue;
    QString m_inputPickerMin;
    QString m_inputPickerMax;
    QString m_inputPickerStep;

    bool m_crashed = false;

    // TLS certificate failure state (cert kept alive until accepted or next load)
    bool m_tlsErrorPending = false;
    QString m_tlsErrorFailingUri;
    QString m_tlsErrorHost;
    QString m_tlsErrorMessage;
    void *m_tlsErrorCert = nullptr; // GTlsCertificate*, ref-held
    // Permission prompt state (geolocation / camera / microphone)
    void *m_pendingPermission = nullptr; // WebKitPermissionRequest*, ref-held
    QString m_permissionHost;
    QString m_permissionType;
    QHash<QString, bool> m_permissionDecisions; // "host|type", session-only
    // Guards the one-shot WebProcess auto-reload (see web-process-terminated
    // handler). Set when an auto-reload is issued; cleared on a successful load
    // so a later, unrelated crash can recover once too. Prevents a reload loop
    // when the page deterministically crashes the WebProcess.
    bool m_autoRecovered = false;
};

QML_DECLARE_TYPE(WPEWebPage)
