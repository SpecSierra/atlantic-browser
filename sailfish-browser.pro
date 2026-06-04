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

data.files = data/prefs.js \
             data/ua-update.json.in \
             data/icon-launcher-browser.png
data.path = /usr/share/atlantic-browser/data

icon.files = data/icon-launcher-browser.png
icon.path = /usr/share/icons/hicolor/86x86/apps

content_blocker.files = data/content-blocker.json
content_blocker.path = /usr/share/atlantic-browser

INSTALLS += desktop dbus_service oneshots data icon content_blocker

usersession.path = /usr/lib/systemd/user/user-session.target.d
usersession.files += 50-sailfish-browser.conf
INSTALLS += usersession

OTHER_FILES += \
    rpm/*.spec
