# Development

## Building on Ubuntu

Arraw needs Qt >= 6.10, LibRaw >= 0.21 and exiv2 >= 0.28. Ubuntu 26.04 or newer
ships a recent enough Qt; on older releases install Qt 6.10 yourself (e.g. with
[aqtinstall](https://github.com/miurahr/aqtinstall)) and point `CMAKE_PREFIX_PATH`
at it.

```bash
sudo apt install build-essential cmake ninja-build pkg-config just \
    qt6-base-dev qt6-base-private-dev qt6-shadertools-dev qt6-tools-dev \
    qt6-image-formats-plugins libraw-dev libexiv2-dev libvulkan-dev catch2
just build
```

To build a `.deb` instead, run `just deb`.

## Building on Fedora

The package list mirrors `packaging/fedora/arraw.spec`.

```bash
sudo dnf install gcc-c++ cmake ninja-build just pkgconf-pkg-config \
    qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtshadertools-devel \
    qt6-qttools-devel qt6-qtimageformats LibRaw-devel exiv2-devel \
    vulkan-headers catch-devel
just build
```

To build an RPM instead, run `just rpm`.

## Building on Windows

Dependencies come from [vcpkg](https://vcpkg.io/) (triplet `x64-windows`), the
compiler is MSVC 2022, and `cmake`, `ninja` and `just` are on `PATH`.

```powershell
vcpkg install qtbase qtshadertools qttools "qtimageformats[core,tiff]" libraw exiv2
# Once, so every fresh build tree finds vcpkg (CMake reads it on first configure)
[Environment]::SetEnvironmentVariable('CMAKE_TOOLCHAIN_FILE', '<vcpkg>\scripts\buildsystems\vcpkg.cmake', 'User')
```

Build from a **Developer PowerShell for VS 2022**, or import the MSVC environment
into a plain PowerShell first:

```powershell
$vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul 2>&1 && set" | % { if ($_ -match '^([^=]+)=(.*)$') { Set-Item "Env:$($matches[1])" $matches[2] } }
just build
```

Troubleshooting:

- `Cannot open include file: 'array'` (C1083): `cl.exe` is on `PATH`, but the
  MSVC environment (`INCLUDE`, `LIB`) isn't loaded. Import it as shown above.
- `CMAKE_TOOLCHAIN_FILE` is ignored: it only seeds a *fresh* build tree, so
  delete `build/` after a configure that ran without it.
