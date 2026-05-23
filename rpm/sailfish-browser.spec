Name:       sailfish-browser

Summary:    Sailfish Browser (WPE WebKit engine)
Version:    2.3.30
Release:    1.wpe1
License:    MPLv2.0
Url:        https://github.com/SpecSierra/sailfish-browser
Source0:    %{name}-%{version}.tar.bz2

# --- Build dependencies (WPE build) ---
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Qml)
BuildRequires:  pkgconfig(Qt5Gui)
BuildRequires:  pkgconfig(Qt5Quick)
BuildRequires:  pkgconfig(Qt5DBus)
BuildRequires:  pkgconfig(Qt5Concurrent)
BuildRequires:  pkgconfig(Qt5Sql)
BuildRequires:  pkgconfig(nemotransferengine-qt5)
BuildRequires:  pkgconfig(mlite5)
BuildRequires:  pkgconfig(qdeclarative5-boostable)
BuildRequires:  pkgconfig(sailfishpolicy)
BuildRequires:  pkgconfig(dsme_dbus_if)
BuildRequires:  pkgconfig(vault) >= 1.0.1
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(gio-2.0)
BuildRequires:  pkgconfig(wpewebkit-2.0)
BuildRequires:  pkgconfig(wpe-1.0)
BuildRequires:  qt5-qttools
BuildRequires:  qt5-qttools-linguist
BuildRequires:  oneshot
BuildRequires:  pkgconfig(gtest)
BuildRequires:  pkgconfig(gmock)

# --- Runtime dependencies ---
Requires: sailfishsilica-qt5 >= 1.2.33
Requires: sailfish-content-graphics
Requires: wpewebkit2 >= 2.52.3
Requires: wpewebkit2-qt5 >= 2.52.3
Requires: wpe-sfos-compat >= 1.0.0
Requires: qt5-plugin-imageformat-ico
Requires: qt5-plugin-imageformat-gif
Requires: qt5-plugin-position-geoclue
Requires: sailjail-launch-approval
Requires: desktop-file-utils
Requires: qt5-qtgraphicaleffects
Requires: nemo-qml-plugin-policy-qt5 >= 0.0.4
Requires: sailfish-policy >= 0.3.31
Requires: libkeepalive >= 1.7.0
Requires: sailfish-components-pickers-qt5 >= 0.1.7
Requires: nemo-qml-plugin-notifications-qt5 >= 1.0.12
Requires: nemo-qml-plugin-connectivity
Requires: jolla-settings >= 0.11.29
Requires: jolla-settings-system >= 1.0.70
Requires: sailfish-policy

# Browser packaging replaces the legacy engine packages
Obsoletes: sailfish-browser-settings <= 2.3.29
Provides:  sailfish-browser-settings > 2.3.29

%{_oneshot_requires_post}

%{!?qtc_qmake5:%define qtc_qmake5 %qmake5}
%{!?qtc_make:%define qtc_make make}

%description
Sailfish Web Browser — WPE WebKit engine build.

WPE WebKit 2.52.3 browser build with the Sailfish UI preserved.
All Silica QML UI is preserved; only the engine layer is swapped.

%package ts-devel
Summary: Translation source for Sailfish browser

%description ts-devel
Translation source for Sailfish Browser

%package tests
Summary: Tests for Sailfish browser
BuildRequires:  pkgconfig(Qt5Test)
Requires:   %{name} = %{version}-%{release}
Requires:   qt5-qtdeclarative-devel-tools
Requires:   qt5-qtdeclarative-import-qttest
Requires:   mce-tools

%description tests
Unit tests and additional data needed for functional tests

%prep
%setup -q -n %{name}-%{version}

%build
# Pass WPE include/lib paths to qmake. The wpewebkit-2.0 pkg-config is
# provided by the wpewebkit2-devel BuildRequires above.
%qtc_qmake5 -r VERSION=%{version} \
    "INCLUDEPATH += /usr/include/wpe-webkit-2.0 /usr/include/wpe-1.0" \
    "LIBS += -lWPEWebKit-2.0 -lwpe-1.0 -lgio-2.0 -lgobject-2.0 -lglib-2.0"
%qtc_make %{?_smp_mflags}

%install
%qmake5_install
chmod +x %{buildroot}/%{_oneshotdir}/*

mkdir -p %{buildroot}/%{_sharedstatedir}/environment/nemo/
cp -f data/70-browser.conf %{buildroot}/%{_sharedstatedir}/environment/nemo/

# Install sailjail profile
install -d %{buildroot}%{_sysconfdir}/sailjail/applications
# NOTE: profile ships from wpe-sfos-build/sailjail/sailfish-browser.profile
# For now install a minimal stub that disables sandboxing pending full profile work
cat > %{buildroot}%{_sysconfdir}/sailjail/applications/sailfish-browser.profile << 'EOF'
[sailfish]
Sandboxing=disabled

[X-Sailjail]
Permissions=Internet;Audio
OrganizationName=org.sailfishos
ApplicationName=sailfish-browser
EOF

%post
/sbin/ldconfig || :

# Upgrade, count is 2 or higher (depending on the number of versions installed)
if [ "$1" -ge 2 ]; then
    %{_bindir}/add-oneshot --all-users --now browser-cleanup-startup-cache || :
    %{_bindir}/add-oneshot --new-users --all-users --late browser-update-default-data || :
fi

%postun
/sbin/ldconfig || :

%files
%license LICENSE.txt
%{_bindir}/%{name}
%{_datadir}/applications/%{name}.desktop
%{_datadir}/%{name}
%{_datadir}/translations/%{name}*.qm
%{_datadir}/translations/settings-%{name}_eng_en.qm
%{_datadir}/dbus-1/services/*.service
%{_oneshotdir}/*
%{_userunitdir}/user-session.target.d/50-sailfish-browser.conf
%dir %{_libdir}/qt5/qml/org/sailfishos/browser
%{_libdir}/libsailfishbrowser.so.*
%exclude %{_libdir}/libsailfishbrowser.so
%{_sharedstatedir}/environment/nemo/70-browser.conf
%{_libexecdir}/jolla-vault/units/vault-browser
%{_datadir}/jolla-vault/units/Browser.json
%{_libdir}/qt5/qml/org/sailfishos/browser/settings
%{_datadir}/jolla-settings/entries/browser.json
%{_datadir}/jolla-settings/pages/browser
%config %{_sysconfdir}/sailjail/applications/sailfish-browser.profile

%files ts-devel
%{_datadir}/translations/source/*.ts

%files tests
%{_datadir}/applications/test-%{name}.desktop
/opt/tests/%{name}
