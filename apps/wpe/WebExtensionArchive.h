/*
 * Atlantic Browser — zip/xpi extraction for extension packages.
 *
 * Qt 5.6's QZipReader cannot read the packages AMO serves. Mozilla's signing
 * pipeline repacks add-ons as *streamed* zips: general-purpose flag bit 3 is
 * set on every entry, which means the local file header carries crc = 0,
 * compressed size = 0 and uncompressed size = 0, with the real values in a data
 * descriptor after the compressed data and in the central directory.
 * QZipReader takes its sizes from the local header, so it reads zero bytes per
 * entry and fails the whole archive.
 *
 * This reader takes everything from the central directory, which is correct for
 * streamed and non-streamed zips alike. It also tolerates a prefix before the
 * archive (the Cr24 header of a .crx) by rebasing every offset on the delta
 * between where the central directory says it is and where it actually is.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <QString>

namespace WebExtensionArchive {

// Extracts `archivePath` into `destinationDir`, creating directories as needed
// (streamed zips carry no explicit directory entries). Entry names that are
// absolute or contain a ".." component are refused outright rather than
// skipped: a package trying to escape its directory is not one to install.
// Every entry's CRC is checked against the central directory.
//
// Returns false and fills `error` with something a user can act on.
bool extract(const QString &archivePath, const QString &destinationDir, QString *error);

} // namespace WebExtensionArchive
