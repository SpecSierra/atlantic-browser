#pragma once
/*
 * MDConfItem stub for WPE build.
 * Returns defaults/no-ops for all operations.
 * SPDX-License-Identifier: LGPL-2.1+
 */
#include <QDebug>
#include <QObject>
#include <QVariant>
#include <QString>

class MDConfItem : public QObject
{
    Q_OBJECT
public:
    explicit MDConfItem(const QString &key, QObject *parent = nullptr)
        : QObject(parent), m_key(key) {}

    // Always returns the default: this is a stub, not a dconf read. Silence
    // here has cost several debugging sessions (adblock startup state, custom
    // home page, close-all-tabs-on-exit), because the call compiles, runs and
    // lies. Warn once per instance so the next one announces itself. Real
    // dconf must go through QML ConfigurationValue, pushed into C++.
    QVariant value(const QVariant &def = QVariant()) const
    {
        if (!m_warned) {
            m_warned = true;
            qWarning() << "MDConfItem stub: read of" << m_key
                       << "returns the default" << def
                       << "- dconf is NOT consulted. Push this value from QML "
                          "(ConfigurationValue -> Q_INVOKABLE) instead.";
        }
        return def;
    }

    void set(const QVariant &) {}
    void unset() {}

signals:
    void valueChanged();

private:
    QString m_key;
    mutable bool m_warned = false;
};
