/****************************************************************************
**
** Copyright (c) 2016 Jolla Ltd.
** Contact: Raine Makelainen <raine.makelaine@jolla.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef WEBENGINE_SETTINGS_H
#define WEBENGINE_SETTINGS_H

#include <QObject>
#include <QVariant>
#include <QSize>

#include "gtest/gtest.h"
#include "gmock/gmock.h"

namespace SailfishOS {

class WebEngineSettings : public QObject
{
    Q_OBJECT

public:
    static WebEngineSettings *instance();
    static void initialize();

    explicit WebEngineSettings(QObject *parent = nullptr) : QObject(parent) {}

    MOCK_METHOD(bool, isInitialized, (), (const));

    MOCK_METHOD(bool, autoLoadImages, (), (const));
    MOCK_METHOD(void, setAutoLoadImages, (bool));

    MOCK_METHOD(bool, javascriptEnabled, (), (const));
    MOCK_METHOD(void, setJavascriptEnabled, (bool));

    MOCK_METHOD(void, setTileSize, (const QSize &));

    MOCK_METHOD(qreal, pixelRatio, (), (const));
    MOCK_METHOD(void, setPixelRatio, (qreal));

    MOCK_METHOD(void, enableProgressivePainting, (bool));
    MOCK_METHOD(void, enableLowPrecisionBuffers, (bool));

    MOCK_METHOD(void, setPreference, (const QString &, const QVariant &));

signals:
    void autoLoadImagesChanged();
    void javascriptEnabledChanged();
    void initialized();
};

}

#endif // WEBENGINE_SETTINGS_H
