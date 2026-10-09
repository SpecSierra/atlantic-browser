/****************************************************************************
**
** Copyright (c) 2015 Jolla Ltd.
** Contact: Dmitry Rozhkov <dmitry.rozhkov@jolla.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BROWSERPATHS_H
#define BROWSERPATHS_H

class QString;

struct BrowserPaths
{
    static QString downloadLocation();
    static QString picturesLocation();
    static QString dataLocation();
    static QString applicationsLocation();
    static QString cacheLocation();
    static QString databasePath();

    // The stock browser's data directory, which Atlantic shared up to 1.6.x.
    // Not created if missing; only the migration and the bookmark import look
    // at it.
    static QString legacyDataLocation();
    // One-shot move of Atlantic's files out of the stock browser's profile.
    // Must run after the application names are set and before anything reads
    // the data directory.
    static void migrateSharedProfile();

    static bool createDirectory(const QString &dirStr);
};

#endif // BROWSERPATHS_H
