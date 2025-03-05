/****************************************************************************
**
** Copyright (c) 2015 Jolla Ltd.
** Contact: Raine Makelainen <raine.makelainen@jolla.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "inputregion.h"

#include <QGuiApplication>
#include <QRegion>
#include <QWindow>

#include <qpa/qplatformnativeinterface.h>

// Hold off actual update to reduce context switches during animations
const int INPUT_REGION_UPDATE_DELAY = 50; // ms

class InputRegionPrivate
{
public:
    InputRegionPrivate(InputRegion *q);

    void scheduleUpdate();
    void update();

    QRect overlayMask;
    QRect selectionStartHandleMask;
    QRect selectionEndHandleMask;
    QWindow *window;
    InputRegion *q_ptr;
    int updateTimerId;

    Q_DECLARE_PUBLIC(InputRegion)
};

InputRegionPrivate::InputRegionPrivate(InputRegion *q)
    : overlayMask(0.0, 0.0, 0.0, 0.0)
    , selectionStartHandleMask(0.0, 0.0, 0.0, 0.0)
    , selectionEndHandleMask(0.0, 0.0, 0.0, 0.0)
    , window(0)
    , q_ptr(q)
    , updateTimerId(0)
{
}

void InputRegionPrivate::update()
{
    Q_Q(InputRegion);
    q->killTimer(updateTimerId);
    updateTimerId = 0;

    if (window) {
        QRect rects[3];
        rects[0] = selectionStartHandleMask;
        rects[1] = selectionEndHandleMask;
        rects[2] = overlayMask;
        QRegion mask;
        mask.setRects(rects, 3);
        window->setMask(mask);
        if (window->handle()) {
            QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
            native->setWindowProperty(window->handle(), QLatin1String("MOUSE_REGION"), mask);
        }
    }
}

void InputRegionPrivate::scheduleUpdate()
{
    Q_Q(InputRegion);

    if (updateTimerId) {
        q->killTimer(updateTimerId);
    }

    updateTimerId = q->startTimer(INPUT_REGION_UPDATE_DELAY);
}


InputRegion::InputRegion(QObject *parent)
    : QObject(parent)
    , d_ptr(new InputRegionPrivate(this))
{
}

const QRect& InputRegion::overlayMask() const
{
    Q_D(const InputRegion);
    return d->overlayMask;
}

void InputRegion::setOverlayMask(const QRect& rect)
{
    Q_D(InputRegion);
    if (d->overlayMask != rect) {
        d->overlayMask = rect;
        d->scheduleUpdate();
        emit overlayMaskChanged();
    }
}

const QRect& InputRegion::selectionStartHandleMask() const
{
    Q_D(const InputRegion);
    return d->selectionStartHandleMask;
}

void InputRegion::setSelectionStartHandleMask(const QRect& rect)
{
    Q_D(InputRegion);
    if (d->selectionStartHandleMask != rect) {
        d->selectionStartHandleMask = rect;
        d->scheduleUpdate();
        emit selectionStartHandleMaskChanged();
    }
}

const QRect& InputRegion::selectionEndHandleMask() const
{
    Q_D(const InputRegion);
    return d->selectionEndHandleMask;
}

void InputRegion::setSelectionEndHandleMask(const QRect& rect)
{
    Q_D(InputRegion);
    if (d->selectionEndHandleMask != rect) {
        d->selectionEndHandleMask = rect;
        d->scheduleUpdate();
        emit selectionEndHandleMaskChanged();
    }
}

QWindow *InputRegion::window() const
{
    Q_D(const InputRegion);
    return d->window;
}

void InputRegion::setWindow(QWindow *window)
{
    Q_D(InputRegion);
    if (d->window != window) {
        d->window = window;
        d->scheduleUpdate();
        emit windowChanged();
    }
}

void InputRegion::timerEvent(QTimerEvent *)
{
    Q_D(InputRegion);
    d->update();
}
