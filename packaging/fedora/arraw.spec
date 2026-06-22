Name:           arraw
Version:        0.2.2
Release:        %{?snapshot_release}%{!?snapshot_release:1}%{?dist}
Summary:        Lightweight RAW photo editor

License:        GPL-3.0-or-later
URL:            https://github.com/janpipek/arraw
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  appstream
BuildRequires:  catch-devel
BuildRequires:  cmake >= 3.21
BuildRequires:  desktop-file-utils
BuildRequires:  gcc-c++
BuildRequires:  ninja-build
BuildRequires:  pkgconfig(lcms2)
BuildRequires:  pkgconfig(libraw) >= 0.21
BuildRequires:  qt6-qtbase-devel >= 6.8
BuildRequires:  qt6-qtbase-private-devel >= 6.8
BuildRequires:  qt6-qtshadertools-devel >= 6.8

Requires:       qt6-qtimageformats%{?_isa}

%description
Arraw is a lightweight RAW photo editor with a real-time GPU preview,
non-destructive XMP sidecars, color-managed output, culling, tone curves,
local adjustments, crop, and straighten tools.

%prep
%autosetup

%build
%cmake -G Ninja \
    -DARRAW_BUILD_TESTS=ON \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON
%cmake_build

%install
%cmake_install

%check
QT_QPA_PLATFORM=offscreen %ctest
desktop-file-validate \
    %{buildroot}%{_datadir}/applications/io.github.janpipek.arraw.desktop
appstreamcli validate --no-net \
    %{buildroot}%{_datadir}/metainfo/io.github.janpipek.arraw.metainfo.xml

%files
%license LICENSE
%doc README.md
%{_bindir}/arraw
%{_datadir}/applications/io.github.janpipek.arraw.desktop
%{_datadir}/metainfo/io.github.janpipek.arraw.metainfo.xml
%{_datadir}/icons/hicolor/*/apps/io.github.janpipek.arraw.png

%changelog
* Sun Jun 22 2026 Jan Pipek <janpipek@users.noreply.github.com> - 0.2.2-1
- Index the tone LUT in the perceptual domain for better shadow detail
- Show a format label (ARW/JPEG/ARW+JPEG) on every filmstrip cell

* Sun Jun 21 2026 Jan Pipek <janpipek@users.noreply.github.com> - 0.2.1-1
- Improve platform-specific Just recipes and add a portable clean task
- Embrace Qt fusion styling

* Sun Jun 21 2026 Jan Pipek <janpipek@users.noreply.github.com> - 0.2.0-1
- Add image rotation, straightening, and EXIF orientation support
- Add vignette and grain effects
- Write digiKam-compatible XMP sidecars and group RAW+JPEG captures
- Fix tone-curve crashes when points change mid-drag

* Sat Jun 20 2026 Jan Pipek <janpipek@users.noreply.github.com> - 0.1.0-1
- Add the first self-hosted Fedora package
