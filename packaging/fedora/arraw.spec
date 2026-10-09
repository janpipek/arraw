Name:           arraw
# A snapshot is a post-release build: the caret sorts it above the release it follows.
%global base_version 0.4.0
Version:        %{base_version}%{?snapshot:^%{snapshot}}
Release:        1%{?dist}
Summary:        RAW photo editor with a GPU preview

License:        GPL-3.0-or-later
URL:            https://github.com/janpipek/arraw
Source0:        %{url}/archive/v%{base_version}/%{name}-%{base_version}.tar.gz

BuildRequires:  appstream
BuildRequires:  catch-devel
BuildRequires:  cmake >= 3.21
BuildRequires:  desktop-file-utils
BuildRequires:  gcc-c++
BuildRequires:  ninja-build
BuildRequires:  pkgconfig(exiv2) >= 0.28
BuildRequires:  pkgconfig(libraw) >= 0.21
BuildRequires:  qt6-qtbase-devel >= 6.10
BuildRequires:  qt6-qtbase-private-devel >= 6.10
# The tests write TIFF through Qt, whose plugin lives here.
BuildRequires:  qt6-qtimageformats
BuildRequires:  qt6-qtshadertools-devel >= 6.10
BuildRequires:  vulkan-headers

# Qt loads image plugins at run time, so the soname dependencies do not find
# them: JPEG and PNG are in qtbase, TIFF is here.
Requires:       qt6-qtimageformats%{?_isa}
Requires:       hicolor-icon-theme
%{?_qt6:Requires: %{_qt6}%{?_isa} = %{_qt6_version}}
# Qt reaches the GPU through the Vulkan loader, which it opens at run time. Without
# one arraw develops on the CPU.
Recommends:     vulkan-loader%{?_isa}
Recommends:     qt6-qtwayland%{?_isa}

%description
Arraw is a RAW processing engine with a Qt desktop application (arraw-ui) and
a command line (arraw-cli). It develops photographs non-destructively into a
linear Rec.2020 working space, previews on the GPU, keeps its edits in XMP
sidecars, and exports to JPEG, PNG or TIFF.

%prep
%autosetup -n %{name}-%{base_version}

%build
# Catch2 comes from catch-devel; FetchContent must not reach for the network.
%cmake -G Ninja \
    -DARRAW_BUILD_TESTS=ON \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON
%cmake_build

%install
%cmake_install

%check
# Neither Koji nor mock has a GPU or a display. The cases that need a Vulkan
# device skip (Catch2 exit 4, registered with SKIP_RETURN_CODE); with no device
# the rest of the suite passes.
# Nothing is excluded by label.
export QT_QPA_PLATFORM=offscreen
%ctest
desktop-file-validate \
    %{buildroot}%{_datadir}/applications/io.github.janpipek.arraw.desktop
appstreamcli validate --no-net \
    %{buildroot}%{_datadir}/metainfo/io.github.janpipek.arraw.metainfo.xml

%files
%license LICENSE
%doc README.md
%{_bindir}/arraw-ui
%{_bindir}/arraw-cli
%{_datadir}/applications/io.github.janpipek.arraw.desktop
%{_datadir}/metainfo/io.github.janpipek.arraw.metainfo.xml
%{_datadir}/icons/hicolor/*/apps/io.github.janpipek.arraw.png
%{_datadir}/icons/hicolor/scalable/apps/io.github.janpipek.arraw.svg

%changelog
* Fri Oct 09 2026 Jan Pipek <janpipek@users.noreply.github.com> - 0.4.0-1
- Package rewrite: the arraw-ui and arraw-cli executables, the desktop entry, the metainfo and the icons
- Some of the old features are temporarily not available.

* Tue Aug 04 2026 Jan Pipek <janpipek@users.noreply.github.com> - 0.3.1-1
- Add the arraw command-line front end with export, preset, and info subcommands (ADR 0049/0050/0051/0053)
- Add arraw export: sidecar-faithful headless batch export
- Add arraw preset list/show/apply, plus in-app rename, delete, and details view
- Add arraw info: report develop state, local masks, and spot counts per photo
- Add Colour Grading: three-zone Oklab toning for colour and B&W images
- Ship the Windows console/GUI executable pair, with PATH and shortcut installation
- Split arraw_core into arraw_engine and arraw_ui (ADR 0049)
- Fix an off-by-one in the installer's PATH cleanup on uninstall

* Mon Jun 29 2026 Jan Pipek <janpipek@users.noreply.github.com> - 0.3.0-1
- Add a Brush Mask type for painting local-adjustment masks on the image
- Add a Black & White treatment with a hue-mixer monochrome conversion
- Add edge-aware Luminance Noise Reduction with Amount and Detail controls
- Add spatial detail controls for local contrast and structure
- Add a mask overlay with an O toggle, default-on, with slider auto-hide
- Pin local-adjustment masks to subject coordinates across crop and rotation
- Add a Windows package build workflow for release ZIPs
- Add a toggle for the History panel

* Sun Jun 28 2026 Jan Pipek <janpipek@users.noreply.github.com> - 0.2.4-1
- Add Oklab perceptual saturation and vibrance with filmic highlight roll-off (on by default)
- Add rating and colour-label filtering to the film strip
- Embed corrected EXIF and XMP metadata in exported images
- Async histogram readback; fix slider-drag jank and stretched-preview regression
- Run export and developed-thumbnail work off the GUI thread to avoid UI hangs
- Add descriptive preset History step labels and keep off-image preset/paste out of per-image History
- Keep the active image in place when leaving is cancelled
- Fix the Colour Noise Reduction shader

* Sat Jun 27 2026 Jan Pipek <janpipek@users.noreply.github.com> - 0.2.3-1
- Add lensfun-backed lens corrections for distortion, vignetting, and chromatic aberration
- Add GPU Colour Noise Reduction with separate Strength and Smoothness controls
- Add a RAW sensor clipping overlay for saturated photosites
- Add Demosaic Algorithm selection with re-decode through the load path
- Add editable descriptive User Metadata in the Info Panel
- Add a History panel with descriptive per-adjustment step labels
- Drive Mask and Spot on-image tools from their adjustment tabs
- Fix local-adjustment mask placement when crop or rotation is active
- Preserve zoom and pan across demosaic re-decodes
- Ship lensfun support and its profile database in Linux release artifacts
- Add an About dialog and refresh installation, shortcut, and README documentation

* Mon Jun 22 2026 Jan Pipek <janpipek@users.noreply.github.com> - 0.2.2-1
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
