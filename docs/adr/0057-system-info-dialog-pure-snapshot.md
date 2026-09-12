# System Info dialog reports GPU, paths, and versions from one pure snapshot function

Bug reports for rendering problems need to know which GPU backend and device
arraw actually picked (OpenGL/Vulkan/D3D11/D3D12/Metal, device name, integrated vs
discrete), and troubleshooting needs the on-disk locations of settings, Develop
Presets, and the thumbnail cache. None of this was surfaced anywhere in the app —
`Help > About...` carries only app/library version and licensing (`AboutDialog`),
and the three paths each live behind their own module (`QSettings`,
`defaultPresetStore()`, `ThumbnailCache`'s cache root) with no single place a user
or a bug report could read them from.

The GPU half is also only knowable at runtime: `QRhi::driverInfo()` and
`QRhi::backend()` only report real values after `ImageViewport`'s `QRhiWidget`
has initialized (`RendererCore::initialize`, normally on first paint) — a
Qt-widget-and-live-GPU dependency that would make any exploratory or
formatting logic hard to unit-test directly.

## What we decided

A standalone `Help > System Info...` dialog, kept separate from `AboutDialog`
(different concern — diagnostics, not app identity/licensing).

**The reportable snapshot is a pure function**, `sysinfo::gather()`
(`src/core/SystemInfo.h/.cpp`, engine layer — no `QtWidgets`), taking
already-known facts rather than reaching for any of them itself:

```cpp
Info gather(
    std::optional<QRhi::Implementation> backend,   // nullopt: QRhi not yet initialized
    std::optional<QRhiDriverInfo> driverInfo,       // nullopt: QRhi not yet initialized
    QString settingsPath,
    QString presetsPath,
    QString cachePath,
    QString appVersion);
```

`Info`'s fields are all display-ready `QString`s (`gpuBackend`, `gpuDeviceName`,
`gpuDeviceType`, `gpuDeviceIds`, the three paths, `appVersion`, `qtVersion`,
`osName`, `cpuArchitecture`, `buildType`) — `SystemInfoDialog` only renders rows
from `Info`, it never touches a `QRhi` type itself. `backendName()` and
`deviceTypeName()` are separate, individually-tested enum-to-label mappers.
`Qt`/OS/CPU/build-type facts (`qVersion()`, `QSysInfo::prettyProductName()`,
`QSysInfo::currentCpuArchitecture()`, the `QT_DEBUG` macro) are read directly
inside `gather()` — they need no live widget or injected value, unlike the paths
and GPU facts, which mirror `PresetStore`'s inject-the-directory pattern
precisely so the caller (`MainWindow`) supplies the one resolved value instead of
`gather()` re-deriving it.

**The three file-location paths come from each module's own single source of
truth**, not re-derived: `defaultPresetStore()` gains a `directoryPath()`
getter, `ThumbnailCache` gains a public static `cacheRootPath()` wrapping its
existing (private, env-var-aware) `cacheRoot()`. Re-deriving
`QStandardPaths::writableLocation(...) + "/presets"` a second time in
`SystemInfo` would be exactly the two-places duplication
[[spot-for-algorithms]] and AGENTS.md's "logic exists exactly once" rule exist
to prevent.

**GPU vendor identity favours `deviceName` over a vendor-ID lookup table.** On
most backends (D3D11, Vulkan, Metal) `QRhiDriverInfo::deviceName` is already a
human-readable vendor+model string (e.g. "NVIDIA GeForce RTX 3080 Ti", "Apple M2
Pro", "Microsoft Basic Render Driver" for WARP); on OpenGL it comes from
`GL_RENDERER`, also normally vendor+model. `deviceName` plus `deviceType`
(integrated/discrete/external/virtual/CPU) is therefore the primary identity
shown; the raw `vendorId`/`deviceId` hex is a secondary "Device IDs" line for
exact bug-report matching, not for naming.

**GPU fields fall back to a placeholder, not a crash**, when `backend`/`driverInfo`
are `nullopt` — `ImageViewport` always exists as `MainWindow`'s central widget, so
this only matters if its `QRhi` failed to initialize before the dialog opens (a
headless/CI edge case in practice, never a normal user path, since Qt paints the
widget at least once before any menu click can be dispatched).

The dialog has a **Copy to Clipboard** button, serializing the same `Info` via
`toPlainText()` (grouped "Label: value" lines) — the dialog's real purpose is
producing something pasteable into a bug report, and full-selecting a grid of
`QLabel`s is fiddly. No "open folder" buttons on the path rows: this stays a
pure diagnostic surface.

## Considered Options

- **Pure `gather()` function + thin rendering dialog (chosen).** `SystemInfo`
  unit-tests headlessly with hand-built `QRhiDriverInfo` values and plain
  strings — no `QApplication`, no real GPU, no `QStandardPaths` involved in the
  test at all. `MainWindow`/`SystemInfoDialog` do the (untested, per the
  existing Qt-widget-glue testing strategy — see ADR 0056) plumbing of pulling
  the live values together once.
- **Query the live viewport and `QStandardPaths` inline in the dialog.**
  Simpler wiring, but the formatting/placeholder logic then can't be
  unit-tested without a real widget and a real (or Null-backend) `QRhi`.
  Rejected for the same reason `PresetStore` takes an injected directory
  instead of calling `QStandardPaths` itself.
- **Fold GPU info into `AboutDialog`.** Fewer menu items, but mixes app
  identity/licensing with runtime diagnostics, and a growing About dialog is a
  worse place to find bug-report info than a dedicated one. Rejected.
- **Friendly PCI-vendor-ID → name lookup table.** Would need ongoing
  maintenance (new vendors) for something `deviceName` already spells out on
  every backend that matters here. Rejected — hex IDs cover the one case that
  actually needs them (exact bug-report matching).
- **"Open folder" button per path row.** Nice convenience, but scope creep
  beyond "basic info", and needs handling for a path that doesn't exist yet.
  Rejected.

## Consequences

- `SystemInfo.{h,cpp}` lives in the engine layer (`ENGINE_SOURCES`) alongside
  `RendererCore`, which already pulls in `Qt6::GuiPrivate` for `<rhi/qrhi.h>` —
  no new dependency category, just a second consumer of it.
- `PresetStore` and `ThumbnailCache` each gain one small public getter
  (`directoryPath()`, `cacheRootPath()`) purely to let `SystemInfo`/`MainWindow`
  read the existing single source of truth instead of restating it.
- The GPU fields can legitimately read "(not yet available)" in odd
  environments (headless CI without a working driver); this is intentional and
  covered by a test, not an oversight.
- Out of scope, deliberately: a vendor-name lookup table, per-path "open
  folder" buttons, and duplicating GPU/version info that already lives in
  `AboutDialog`.

*The code is `src/core/SystemInfo.{h,cpp}` and `tests/test_SystemInfo.cpp`
(the pure snapshot), with `src/ui/SystemInfoDialog.{h,cpp}` and the
`MainWindow` `Help > System Info...` wiring landing in the same change.*
