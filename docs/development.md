# Development

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
