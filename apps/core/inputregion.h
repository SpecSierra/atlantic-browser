/****************************************************************************
**
** Copyright (c) 2015 Jolla Ltd.
** Contact: Raine Makelainen <raine.makelainen@jolla.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef INPUTREGION_H
#define INPUTREGION_H

#include <QObject>
#include <QRect>
#include <qqml.h>

class QWindow;
class InputRegionPrivate;

class InputRegion : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QRect overlayMask READ overlayMask WRITE setOverlayMask NOTIFY overlayMaskChanged)
    Q_PROPERTY(QRect selectionStartHandleMask READ selectionStartHandleMask WRITE setSelectionStartHandleMask NOTIFY selectionStartHandleMaskChanged FINAL)
    Q_PROPERTY(QRect selectionEndHandleMask READ selectionEndHandleMask WRITE setSelectionEndHandleMask NOTIFY selectionEndHandleMaskChanged FINAL)
    Q_PROPERTY(QRect closeButtonMask READ closeButtonMask WRITE setCloseButtonMask NOTIFY closeButtonMaskChanged)
    Q_PROPERTY(QWindow *window READ window WRITE setWindow NOTIFY windowChanged FINAL)
    Q_PROPERTY(int orientation READ orientation WRITE setOrientation NOTIFY orientationChanged)

public:
    InputRegion(QObject *parent = 0);

    const QRect& overlayMask() const;
    void setOverlayMask(const QRect& rect);
    const QRect& selectionStartHandleMask() const;
    void setSelectionStartHandleMask(const QRect& rect);
    const QRect& selectionEndHandleMask() const;
    void setSelectionEndHandleMask(const QRect& rect);
    const QRect& closeButtonMask() const;
    void setCloseButtonMask(const QRect& rect);

    QWindow *window() const;
    void setWindow(QWindow *window);

    int orientation() const;
    void setOrientation(int orientation);

signals:
    void overlayMaskChanged();
    void selectionStartHandleMaskChanged();
    void selectionEndHandleMaskChanged();
    void closeButtonMaskChanged();
    void windowChanged();
    void orientationChanged();

protected:
    void timerEvent(QTimerEvent *) override;

private:
    InputRegionPrivate *d_ptr;
    Q_DISABLE_COPY(InputRegion)
    Q_DECLARE_PRIVATE(InputRegion)
};

QML_DECLARE_TYPE(InputRegion)

#endif
