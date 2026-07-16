/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.2
import Sailfish.Silica 1.0

// Native pickers for HTML5 <input type=date|month|week|time|datetime-local|color>.
// WebKit has no native WPE picker for these (createColorPicker/createDateTimePicker
// return nullptr in the WPE port), so the engine's kInputPickerBridge userscript
// forwards each activation to WPEWebPage, which raises inputPicker* on its
// contentItem. This non-visual item watches that state and drives the matching
// Silica dialog, then writes the chosen value back with resolveInputPicker()
// (or cancelInputPicker() on dismiss). Same NOTIFY-property bridge as
// SelectMenuOverlay.
Item {
    id: root

    property var webView
    readonly property var page: (webView && webView.contentItem) ? webView.contentItem : null

    // Rising edge of inputPickerActive opens the picker; _busy guards against
    // re-entrancy while a dialog is on the stack.
    property bool _busy: false
    readonly property bool active: page ? page.inputPickerActive : false
    onActiveChanged: {
        if (active && !_busy)
            openPicker()
    }

    // ---- formatting helpers ----
    function pad(n, width) {
        var s = "" + Math.abs(n)
        while (s.length < (width || 2))
            s = "0" + s
        return s
    }

    // ISO-8601 week number + week-numbering year for a local date. Computed in
    // UTC so DST transitions never shift the day. Both `date` and `firstThursday`
    // land on a Thursday, so their difference is an exact multiple of 7 days.
    function isoWeek(d) {
        var date = new Date(Date.UTC(d.getFullYear(), d.getMonth(), d.getDate()))
        var dayNum = (date.getUTCDay() + 6) % 7            // Mon=0 .. Sun=6
        date.setUTCDate(date.getUTCDate() - dayNum + 3)    // Thursday of this week
        var firstThursday = new Date(Date.UTC(date.getUTCFullYear(), 0, 4))
        var firstDayNum = (firstThursday.getUTCDay() + 6) % 7
        firstThursday.setUTCDate(firstThursday.getUTCDate() - firstDayNum + 3)
        var week = 1 + Math.round((date - firstThursday) / (7 * 86400000))
        return { year: date.getUTCFullYear(), week: week }
    }

    // Monday (local date) of ISO week `week` in week-numbering year `year`.
    function isoWeekToMonday(year, week) {
        var jan4 = new Date(Date.UTC(year, 0, 4))
        var jan4dow = (jan4.getUTCDay() + 6) % 7
        var week1Mon = new Date(jan4)
        week1Mon.setUTCDate(jan4.getUTCDate() - jan4dow + (week - 1) * 7)
        return new Date(week1Mon.getUTCFullYear(), week1Mon.getUTCMonth(), week1Mon.getUTCDate())
    }

    function twoHex(v) {
        var n = Math.max(0, Math.min(255, Math.round(v * 255)))
        var s = n.toString(16)
        return s.length < 2 ? "0" + s : s
    }
    function colorToHex(c) {
        return "#" + twoHex(c.r) + twoHex(c.g) + twoHex(c.b)
    }

    // ---- initial-value parsing (falls back to "now" when the field is empty) ----
    function initialDate(type, val) {
        var now = new Date()
        if (!val)
            return now
        var m
        if (type === "week") {
            m = /^(\d{4})-W(\d{2})$/.exec(val)
            if (m)
                return isoWeekToMonday(parseInt(m[1], 10), parseInt(m[2], 10))
        } else if (type === "month") {
            m = /^(\d{4})-(\d{2})$/.exec(val)
            if (m)
                return new Date(parseInt(m[1], 10), parseInt(m[2], 10) - 1, 1)
        } else { // date, datetime-local (date part)
            m = /^(\d{4})-(\d{2})-(\d{2})/.exec(val)
            if (m)
                return new Date(parseInt(m[1], 10), parseInt(m[2], 10) - 1, parseInt(m[3], 10))
        }
        return now
    }
    function initialTime(val) {
        var now = new Date()
        // time "HH:MM[:SS]" or datetime-local "...THH:MM"
        var m = /(?:^|T)(\d{2}):(\d{2})/.exec(val || "")
        if (m)
            return { hour: parseInt(m[1], 10), minute: parseInt(m[2], 10) }
        return { hour: now.getHours(), minute: now.getMinutes() }
    }

    function formatDate(d)  { return d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" + pad(d.getDate()) }
    function formatMonth(d) { return d.getFullYear() + "-" + pad(d.getMonth() + 1) }
    function formatWeek(d)  { var w = isoWeek(d); return w.year + "-W" + pad(w.week) }

    // ---- result plumbing ----
    function resolve(value) {
        if (page)
            page.resolveInputPicker(value)
        _busy = false
    }
    function cancel() {
        if (page)
            page.cancelInputPicker()
        _busy = false
    }

    // ---- dispatch ----
    function openPicker() {
        if (!page)
            return
        _busy = true
        var type = page.inputPickerType
        var val = page.inputPickerValue
        if (type === "color")
            openColor(val)
        else if (type === "time")
            openTime(val, function (h, m) { resolve(pad(h) + ":" + pad(m)) })
        else if (type === "datetime-local")
            openDateTimeLocal(val)
        else
            openDate(type, val) // date | month | week
    }

    function openDate(type, val) {
        var op = pageStack.animatorPush(dateDialogComponent, { date: initialDate(type, val) })
        op.pageCompleted.connect(function (dialog) {
            dialog.accepted.connect(function () {
                var d = dialog.date
                if (type === "month")
                    resolve(formatMonth(d))
                else if (type === "week")
                    resolve(formatWeek(d))
                else
                    resolve(formatDate(d))
            })
            dialog.rejected.connect(function () { cancel() })
        })
    }

    function openTime(val, done) {
        var t = initialTime(val)
        var op = pageStack.animatorPush(timeDialogComponent, { hour: t.hour, minute: t.minute })
        op.pageCompleted.connect(function (dialog) {
            dialog.accepted.connect(function () { done(dialog.hour, dialog.minute) })
            dialog.rejected.connect(function () { cancel() })
        })
    }

    // datetime-local: pick the date, then chain into the time picker; combine
    // both into "YYYY-MM-DDTHH:MM". Cancelling either step cancels the whole.
    function openDateTimeLocal(val) {
        var op = pageStack.animatorPush(dateDialogComponent, { date: initialDate("datetime-local", val) })
        op.pageCompleted.connect(function (dialog) {
            dialog.accepted.connect(function () {
                var datePart = formatDate(dialog.date)
                openTime(val, function (h, m) {
                    resolve(datePart + "T" + pad(h) + ":" + pad(m))
                })
            })
            dialog.rejected.connect(function () { cancel() })
        })
    }

    function openColor(val) {
        var props = {}
        if (/^#[0-9a-fA-F]{6}$/.test(val || ""))
            props.color = val
        var op = pageStack.animatorPush(colorDialogComponent, props)
        op.pageCompleted.connect(function (dialog) {
            dialog.accepted.connect(function () { resolve(colorToHex(dialog.color)) })
            dialog.rejected.connect(function () { cancel() })
        })
    }

    Component { id: dateDialogComponent;  DatePickerDialog {} }
    Component { id: timeDialogComponent;  TimePickerDialog {} }
    Component { id: colorDialogComponent; ColorPickerDialog {} }
}
