/****************************************************************************
**
** Copyright (C) 2015 Jolla Ltd.
** Contact: Piotr Tworek <piotr.tworek@jolla.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef qmozwindow_h
#define qmozwindow_h

#include <QObject>
#include <QSize>
#include <QRect>

#include "gtest/gtest.h"
#include "gmock/gmock.h"

class QMozWindowListener;

class QMozWindow : public QObject
{
    Q_OBJECT

public:
    explicit QMozWindow(const QSize &size, QObject *parent = nullptr)
        : QObject(parent)
    {
        Q_UNUSED(size);
    }

    MOCK_METHOD(void, setSize, (QSize));
    MOCK_METHOD(QSize, size, (void));
    MOCK_METHOD(void, setContentOrientation, (Qt::ScreenOrientation));
    MOCK_METHOD(Qt::ScreenOrientation, contentOrientation, ());
    MOCK_METHOD(Qt::ScreenOrientation, pendingOrientation, ());
    MOCK_METHOD(void, getPlatformImage, (int*, int*));
    MOCK_METHOD(void, suspendRendering, (void));
    MOCK_METHOD(void, resumeRendering, (void));
    MOCK_METHOD(void, scheduleUpdate, (void));
    MOCK_METHOD(bool, readyToPaint, (void));
    MOCK_METHOD(bool, setReadyToPaint, (bool));

signals:
    void pendingOrientationChanged(Qt::ScreenOrientation orientation);
    void orientationChangeFiltered(Qt::ScreenOrientation orientation);
    void requestGLContext();
    void initialized();
    void drawOverlay(QRect);
    void compositorCreated();
    void compositingFinished();
};

#endif /* qmozwindow_h */
