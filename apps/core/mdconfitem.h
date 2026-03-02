#pragma once
/*
 * MDConfItem stub for WPE build.
 * Returns defaults/no-ops for all operations.
 * SPDX-License-Identifier: LGPL-2.1+
 */
#include <QObject>
#include <QVariant>
#include <QString>

class MDConfItem : public QObject
{
    Q_OBJECT
public:
    explicit MDConfItem(const QString &key, QObject *parent = nullptr)
        : QObject(parent), m_key(key) {}

    QVariant value(const QVariant &def = QVariant()) const { return def; }
    void set(const QVariant &) {}
    void unset() {}

signals:
    void valueChanged();

private:
    QString m_key;
};
