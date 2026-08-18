TEMPLATE = subdirs
SUBDIRS += apps settings

tests.depends = apps

# The .desktop file
desktop.files = sailfish-browser.desktop
desktop.path = /usr/share/applications

dbus_service.files = org.atlantic.browser.service \
                     org.atlantic.browser.ui.service
dbus_service.path = /usr/share/dbus-1/services

oneshots.files = oneshot.d/browser-cleanup-startup-cache \
                 oneshot.d/browser-update-default-data
oneshots.path  = /usr/lib/oneshot.d

data.files = data/icon-launcher-browser.png
data.path = /usr/share/atlantic-browser/data

# Performance intervention rules (read by WPEWebPage at page setup). Note the
# engine's build-rpms-native.sh stages this file itself — qmake INSTALLS never
# runs in the CI build — so both places must be updated together.
perfrules.files = data/perf-interventions.json
perfrules.path = /usr/share/atlantic-browser

icon.files = data/icon-launcher-browser.png
icon.path = /usr/share/icons/hicolor/86x86/apps

INSTALLS += desktop dbus_service oneshots data perfrules icon

usersession.path = /usr/lib/systemd/user/user-session.target.d
usersession.files += 50-sailfish-browser.conf
INSTALLS += usersession

OTHER_FILES += \
    rpm/*.spec
