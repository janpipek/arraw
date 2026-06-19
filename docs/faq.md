# arraw FAQ

User-facing answers to questions that come up while *running* arraw (not building
it — for that see the [Windows](windows-build.md) and [Linux](linux-build.md)
guides).

---

## My laptop has a discrete GPU, but arraw seems to run on the integrated one

On a laptop with switchable graphics (an Intel/AMD integrated GPU plus a discrete
NVIDIA or AMD one), arraw may end up rendering on the **integrated** GPU. You'll
see the integrated GPU at full load while the discrete card sits at 0%. arraw
renders through Qt's RHI layer — **D3D11 on Windows, OpenGL on Linux** (see
[ADR 0006](adr/0006-rhi-migration-single-renderer-core.md)) — and which physical
GPU that lands on is decided by the OS/driver, which often defaults to the
integrated adapter to save power.

### First, confirm which GPU is actually in use

Don't trust Task Manager's per-GPU graphs alone — ask Qt directly. Run arraw from
a terminal with the RHI logging category on; it prints the adapter it selected at
startup.

**Windows** (PowerShell):
```powershell
$env:QT_LOGGING_RULES = "qt.rhi.general=true"
.\arraw.exe
```

**Linux**:
```bash
QT_LOGGING_RULES="qt.rhi.general=true" ./arraw
```

Look for a line naming the chosen adapter (e.g. `Using ... adapter: Intel(R) ...`
vs `NVIDIA ...`). That is the ground truth.

> On Windows, arraw is a GUI-subsystem binary with no attached console, so
> redirect stderr to see the log: `arraw.exe 2> rhi.txt`.

### Windows — force the discrete GPU

Pick whichever you prefer; the OS setting and the vendor control panel do the same
thing.

- **Windows Graphics settings** — Settings → System → Display → **Graphics** → add
  `arraw.exe` → **Options** → **High performance**.
- **NVIDIA Control Panel** — Manage 3D settings → **Program Settings** → add
  `arraw.exe` → "Preferred graphics processor" → **High-performance NVIDIA
  processor**.

To test without changing any system setting, force the DXGI adapter index for one
run (find the NVIDIA index from the RHI log above; it's usually `1`):

```powershell
$env:QT_D3D_ADAPTER_INDEX = "1"
.\arraw.exe
```

### Linux — force the discrete GPU

For an NVIDIA card on the PRIME render-offload setup, set these before launching:

```bash
__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia ./arraw
```

On an AMD/Mesa PRIME setup, use:

```bash
DRI_PRIME=1 ./arraw
```

To make it permanent, add the environment variables to a launcher (e.g. a
`.desktop` file's `Exec=` line, or a shell wrapper). Some desktop environments also
expose a "Run with discrete GPU" / "Launch using Dedicated Graphics Card" entry in
the application's right-click menu, which sets the same variables for you.

> Re-run the verification step after changing any setting to confirm the discrete
> GPU is now selected. Note that a different GPU vendor can subtly change
> rendering output — this is expected and is exactly what the golden-image tests in
> [ADR 0005](adr/0005-golden-image-tests-tolerance-policy.md) exist to catch.
