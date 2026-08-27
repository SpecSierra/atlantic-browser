/*
 * Atlantic Browser — zip/xpi extraction for extension packages.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "WebExtensionArchive.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <zlib.h>

namespace {

// Little-endian readers over a QByteArray, bounds-checked by the caller.
quint16 readU16(const QByteArray &data, int offset)
{
    return quint16(quint8(data.at(offset)))
        | (quint16(quint8(data.at(offset + 1))) << 8);
}

quint32 readU32(const QByteArray &data, int offset)
{
    return quint32(quint8(data.at(offset)))
        | (quint32(quint8(data.at(offset + 1))) << 8)
        | (quint32(quint8(data.at(offset + 2))) << 16)
        | (quint32(quint8(data.at(offset + 3))) << 24);
}

quint64 readU64(const QByteArray &data, int offset)
{
    return quint64(readU32(data, offset)) | (quint64(readU32(data, offset + 4)) << 32);
}

const quint32 kEndOfCentralDirectory = 0x06054b50; // PK\5\6
const quint32 kCentralFileHeader      = 0x02014b50; // PK\1\2
const quint32 kLocalFileHeader        = 0x04034b50; // PK\3\4
const quint32 kZip64Sentinel          = 0xFFFFFFFF;

// The EOCD is last, but a variable-length comment may follow it, so scan back.
int findEndOfCentralDirectory(const QByteArray &data)
{
    const int maxComment = 0xFFFF;
    const int lowest = qMax(0, data.size() - maxComment - 22);
    for (int offset = data.size() - 22; offset >= lowest; --offset) {
        if (readU32(data, offset) == kEndOfCentralDirectory)
            return offset;
    }
    return -1;
}

// A name is rejected, not sanitised: anything trying to write outside the
// destination is a reason to refuse the package, not to silently rename it.
bool isSafeEntryName(const QString &name)
{
    if (name.isEmpty() || name.startsWith(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        return false;
    if (name.size() > 1 && name.at(1) == QLatin1Char(':')) // drive letter
        return false;
    const QStringList parts = name.split(QLatin1Char('/'));
    for (const QString &part : parts) {
        if (part == QLatin1String(".."))
            return false;
    }
    return true;
}

bool inflateRaw(const QByteArray &input, quint64 expectedSize, QByteArray *output, QString *error)
{
    output->resize(int(expectedSize));
    if (expectedSize == 0)
        return true;

    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    // Negative window bits: raw deflate, no zlib or gzip wrapper — which is
    // what a zip entry holds.
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        *error = QStringLiteral("could not start decompression");
        return false;
    }

    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.constData()));
    stream.avail_in = uInt(input.size());
    stream.next_out = reinterpret_cast<Bytef *>(output->data());
    stream.avail_out = uInt(expectedSize);

    const int result = inflate(&stream, Z_FINISH);
    const uLong written = stream.total_out;
    inflateEnd(&stream);

    if (result != Z_STREAM_END || written != expectedSize) {
        *error = QStringLiteral("compressed data is damaged");
        return false;
    }
    return true;
}

} // namespace

bool WebExtensionArchive::extract(const QString &archivePath, const QString &destinationDir,
                                  QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    QFile archive(archivePath);
    if (!archive.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot open %1: %2")
                        .arg(QFileInfo(archivePath).fileName(), archive.errorString()));
    const QByteArray data = archive.readAll();
    archive.close();

    if (data.size() < 22)
        return fail(QStringLiteral("%1 is too small to be a package")
                        .arg(QFileInfo(archivePath).fileName()));

    const int eocd = findEndOfCentralDirectory(data);
    if (eocd < 0)
        return fail(QStringLiteral("%1 is not a zip archive")
                        .arg(QFileInfo(archivePath).fileName()));

    const quint16 entryCount = readU16(data, eocd + 10);
    const quint32 directorySize = readU32(data, eocd + 12);
    const quint32 directoryOffset = readU32(data, eocd + 16);

    if (entryCount == 0)
        return fail(QStringLiteral("the package is empty"));
    if (directoryOffset == kZip64Sentinel || directorySize == kZip64Sentinel
        || entryCount == 0xFFFF) {
        return fail(QStringLiteral("zip64 packages are not supported"));
    }

    // A .crx (or any prefixed archive) shifts everything by the size of its
    // header; the central directory's own recorded position tells us by how
    // much. Zero for a plain zip.
    const qint64 delta = qint64(eocd) - qint64(directoryOffset) - qint64(directorySize);
    if (delta < 0
        || qint64(directoryOffset) + delta + qint64(directorySize) > qint64(data.size())) {
        return fail(QStringLiteral("the package's directory is out of bounds"));
    }

    if (!QDir().mkpath(destinationDir))
        return fail(QStringLiteral("cannot create %1").arg(destinationDir));
    const QString destinationRoot = QDir(destinationDir).absolutePath();

    qint64 cursor = qint64(directoryOffset) + delta;
    int extracted = 0;

    for (int i = 0; i < entryCount; ++i) {
        if (cursor + 46 > data.size() || readU32(data, int(cursor)) != kCentralFileHeader)
            return fail(QStringLiteral("the package's directory is damaged"));

        const quint16 method = readU16(data, int(cursor) + 10);
        const quint32 crc = readU32(data, int(cursor) + 16);
        quint64 compressedSize = readU32(data, int(cursor) + 20);
        quint64 uncompressedSize = readU32(data, int(cursor) + 24);
        const quint16 nameLength = readU16(data, int(cursor) + 28);
        const quint16 extraLength = readU16(data, int(cursor) + 30);
        const quint16 commentLength = readU16(data, int(cursor) + 32);
        quint64 localOffset = readU32(data, int(cursor) + 42);

        const qint64 nameStart = cursor + 46;
        if (nameStart + nameLength + extraLength + commentLength > data.size())
            return fail(QStringLiteral("the package's directory is damaged"));

        const QString name = QString::fromUtf8(data.constData() + nameStart, nameLength);

        // Sizes above 4 GiB live in the zip64 extra field. Extensions never hit
        // this, but read it rather than silently truncating if one ever does.
        if (compressedSize == kZip64Sentinel || uncompressedSize == kZip64Sentinel
            || localOffset == kZip64Sentinel) {
            qint64 extra = nameStart + nameLength;
            const qint64 extraEnd = extra + extraLength;
            bool found = false;
            while (extra + 4 <= extraEnd) {
                const quint16 headerId = readU16(data, int(extra));
                const quint16 fieldSize = readU16(data, int(extra) + 2);
                if (headerId == 0x0001) {
                    qint64 field = extra + 4;
                    if (uncompressedSize == kZip64Sentinel && field + 8 <= extraEnd) {
                        uncompressedSize = readU64(data, int(field));
                        field += 8;
                    }
                    if (compressedSize == kZip64Sentinel && field + 8 <= extraEnd) {
                        compressedSize = readU64(data, int(field));
                        field += 8;
                    }
                    if (localOffset == kZip64Sentinel && field + 8 <= extraEnd)
                        localOffset = readU64(data, int(field));
                    found = true;
                    break;
                }
                extra += 4 + fieldSize;
            }
            if (!found)
                return fail(QStringLiteral("%1 has no zip64 record").arg(name));
        }

        cursor = nameStart + nameLength + extraLength + commentLength;

        if (name.endsWith(QLatin1Char('/'))) // directory entry
            continue;
        if (!isSafeEntryName(name))
            return fail(QStringLiteral("the package contains an unsafe path: %1").arg(name));

        // Sizes and offsets come from the central directory; the local header is
        // read only to find where its variable-length fields end, because a
        // streamed entry's copies of them are all zero.
        const qint64 local = qint64(localOffset) + delta;
        if (local < 0 || local + 30 > data.size()
            || readU32(data, int(local)) != kLocalFileHeader) {
            return fail(QStringLiteral("%1 has a damaged header").arg(name));
        }
        const quint16 localNameLength = readU16(data, int(local) + 26);
        const quint16 localExtraLength = readU16(data, int(local) + 28);
        const qint64 dataStart = local + 30 + localNameLength + localExtraLength;
        if (dataStart + qint64(compressedSize) > data.size())
            return fail(QStringLiteral("%1 runs past the end of the package").arg(name));

        const QByteArray compressed = QByteArray::fromRawData(data.constData() + dataStart,
                                                              int(compressedSize));
        QByteArray contents;
        if (method == 0) {
            if (compressedSize != uncompressedSize)
                return fail(QStringLiteral("%1 has inconsistent sizes").arg(name));
            contents = compressed;
        } else if (method == 8) {
            QString inflateError;
            if (!inflateRaw(compressed, uncompressedSize, &contents, &inflateError))
                return fail(QStringLiteral("%1: %2").arg(name, inflateError));
        } else {
            return fail(QStringLiteral("%1 uses an unsupported compression method (%2)")
                            .arg(name).arg(method));
        }

        if (crc != 0
            && crc32(0, reinterpret_cast<const Bytef *>(contents.constData()),
                     uInt(contents.size())) != crc) {
            return fail(QStringLiteral("%1 failed its checksum").arg(name));
        }

        const QString target = QDir::cleanPath(QDir(destinationRoot).absoluteFilePath(name));
        if (!target.startsWith(destinationRoot + QLatin1Char('/')))
            return fail(QStringLiteral("the package contains an unsafe path: %1").arg(name));

        // Streamed archives carry no directory entries, so every parent has to
        // be created on the way.
        if (!QDir().mkpath(QFileInfo(target).path()))
            return fail(QStringLiteral("cannot create a directory for %1").arg(name));

        QFile out(target);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return fail(QStringLiteral("cannot write %1: %2").arg(name, out.errorString()));
        if (out.write(contents) != contents.size())
            return fail(QStringLiteral("cannot write %1: %2").arg(name, out.errorString()));
        out.close();
        ++extracted;
    }

    if (extracted == 0)
        return fail(QStringLiteral("the package contains no files"));

    qDebug() << "[WEBEXT] extracted" << extracted << "file(s) from"
             << QFileInfo(archivePath).fileName();
    return true;
}
