/*
 * Minimal stub header for DeclarativeWebContainer used by tab models and browser service.
 * WPE replacement: browsing context IDs are not used, methods return 0/empty.
 * SPDX-License-Identifier: LGPL-2.1+
 */
#pragma once
#include <QObject>
#include <QPointer>
#include <QString>

class DeclarativeWebContainer : public QObject
{
    Q_OBJECT
public:
    explicit DeclarativeWebContainer(QObject *parent = nullptr) : QObject(parent) {}

    static DeclarativeWebContainer *instance() {
        static DeclarativeWebContainer s_instance;
        return &s_instance;
    }

    // Gecko-specific: browsing context to tab ID mapping — stub returns 0
    int tabId(uint32_t) const { return 0; }
    // Return previously active tab ID — stub returns 0
    int previouslyUsedTabId() const { return 0; }

    // Browser D-Bus service stubs
    void requestTabWithOwnerAsync(int /*tabId*/, const QString & /*url*/, qint64 /*pid*/, void * /*ctx*/) {}
    int  tabOwner(int) const { return 0; }
    void closeTab(int) {}

signals:
    void requestTabWithOwnerAsyncResult(int tabId, void *context);
};
