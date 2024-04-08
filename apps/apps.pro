TEMPLATE = subdirs
SUBDIRS += lib browser captiveportal

browser.depends = lib
captiveportal.depends = lib
