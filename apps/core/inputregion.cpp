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
#include <QScreen>
#include <QDebug>

#include <qpa/qplatformnativeinterface.h>

// Hold off actual update to reduce context switches during animations
const int INPUT_REGION_UPDATE_DELAY = 50; // ms

class InputRegionPrivate
{
public:
    InputRegionPrivate(InputRegion *q);

    QRect translateRect(QRect input);
    void scheduleUpdate();
    void update();

    QRect overlayMask;
    QRect selectionStartHandleMask;
    QRect selectionEndHandleMask;
    QRect closeButtonMask;
    QWindow *window;
    InputRegion *q_ptr;
    int updateTimerId;
    Qt::ScreenOrientation orientation;

    Q_DECLARE_PUBLIC(InputRegion)
};

InputRegionPrivate::InputRegionPrivate(InputRegion *q)
    : overlayMask(0.0, 0.0, 0.0, 0.0)
    , selectionStartHandleMask(0.0, 0.0, 0.0, 0.0)
    , selectionEndHandleMask(0.0, 0.0, 0.0, 0.0)
    , closeButtonMask(0.0, 0.0, 0.0, 0.0)
    , window(0)
    , q_ptr(q)
    , updateTimerId(0)
    , orientation(Qt::PortraitOrientation)
{
}

QRect InputRegionPrivate::translateRect(QRect input)
{
    QRect result = input;
    QScreen *screen = qApp->primaryScreen();
    if (!screen) {
        return result;
    }

    int angle = screen->angleBetween(orientation, screen->primaryOrientation());

    if (angle == 180 || angle == -180) {
        result.moveTop(screen->size().height() - input.y() - input.height());
        result.moveLeft(screen->size().width() - input.x() - input.width());
    } else if (qAbs(angle) == 270 || qAbs(angle) == 90) {
        result.setWidth(input.height());
        result.setHeight(input.width());
    }

    if (angle == 270 || angle == -90) {
        result.moveLeft(input.y());
        result.moveTop(screen->size().height() - input.width() - input.x());
    } else if (angle == -270 || angle == 90) {
        result.moveTop(input.x());
        result.moveLeft(screen->size().width() - input.height() - input.y());
    }

    return result;
}

void InputRegionPrivate::update()
{
    Q_Q(InputRegion);
    q->killTimer(updateTimerId);
    updateTimerId = 0;

    if (window) {
        QRect rects[4];
        rects[0] = translateRect(selectionStartHandleMask);
        rects[1] = translateRect(selectionEndHandleMask);
        rects[2] = translateRect(overlayMask);
        rects[3] = translateRect(closeButtonMask);
        QRegion mask;
        mask.setRects(rects, 4);
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

const QRect& InputRegion::closeButtonMask() const
{
    Q_D(const InputRegion);
    return d->closeButtonMask;
}

void InputRegion::setCloseButtonMask(const QRect& rect)
{
    Q_D(InputRegion);
    if (d->closeButtonMask != rect) {
        d->closeButtonMask = rect;
        d->scheduleUpdate();
        emit closeButtonMaskChanged();
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

int InputRegion::orientation() const
{
    Q_D(const InputRegion);
    return d->orientation;
}

void InputRegion::setOrientation(int orientation)
{
    Q_D(InputRegion);
    if (d->orientation != orientation) {
        d->orientation = static_cast<Qt::ScreenOrientation>(orientation);
        d->scheduleUpdate();
        emit orientationChanged();
    }
}

void InputRegion::timerEvent(QTimerEvent *)
{
    Q_D(InputRegion);
    d->update();
}
