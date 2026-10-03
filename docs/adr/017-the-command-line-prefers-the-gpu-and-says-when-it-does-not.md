# The command line prefers the GPU, and says when it does not

[ADR 015](015-a-checkpoints-pixels-may-live-on-a-device.md) put pixels on a
device and required that "fallback must not be able to hide a missing GPU". The
[GPU develop plan](../ideas/gpu-develop-plan.md) then built `developOnGpu`, and
`arraw-cli export` was the first caller that had to decide when to use it. The
decision is small in code and large in trust: a user who asked for the GPU and
silently got the CPU has been misled about speed, and a user whose overnight
batch died on one oversize frame has lost the night. This ADR records where the
line between those falls.

## Decision

**`--device auto|gpu|cpu` chooses the device, and `--gpu-backend` chooses the
API.** They are different questions: `--device gpu` says nothing about Vulkan
against OpenGL, and `--gpu-backend vulkan` says nothing about whether the GPU is
mandatory. `auto` is the default, so a plain `export` uses the GPU when there is
one. `--gpu-backend` takes `vulkan`, `opengl`, `d3d11`, `d3d12`, `metal` and
defaults to the platform's (`defaultGpuBackend()`). `gpu-test --backend` became
`--gpu-backend` for consistency, and `--backend` remains an alias.
`--allow-software` accepts a software rasteriser.

**One device per batch.** `export` creates a single `GpuContext` on the main
thread, uses it for every input and releases every `DeviceImage` before it is
destroyed, as ADR 015 requires. Nothing here adds threads.

**`auto` falls back, says so once, and never silently.**

- If the device cannot be created, is a software rasteriser (llvmpipe, lavapipe,
  WARP) and `--allow-software` was not given, or `ARRAW_DISABLE_GPU` is set, the
  whole batch runs on the CPU and one `Notice::GpuFallback` says why. A software
  rasteriser is refused because it is usually slower than the CPU path, so
  accepting it would cost the user the speed they were choosing `auto` for.
- If the GPU cannot develop one input (an image beyond the device's texture
  limit, say), that input is retried on the CPU with a warning, and the next
  input tries the GPU again. One large frame does not move the whole batch.
- If the device is lost, the rest of the batch runs on the CPU. A lost device
  does not come back, so retrying it per input would only repeat the failure.

**`gpu` never falls back.** Failure to create the device is `Failed` (exit 1)
before any input is touched. A GPU failure on one input fails that input, as any
other export failure does, and the batch continues. `--device gpu` together with
`ARRAW_DISABLE_GPU` is a usage error (exit 2), because the two instructions
contradict each other and one of them has to lose silently otherwise. Software
rasterisers are refused here too unless `--allow-software` is given.

**`cpu` is what export did before.** It asks only for `ApplicationKind::Core`,
so no platform plugin loads, and help and usage errors still load none.

**`ARRAW_DISABLE_GPU` means "keep the graphics stack out of this process".** Any
value other than `0` disables it. Under `auto` it is a fallback with a notice;
under `cpu` it is redundant; under `gpu` it is a usage error.

**Export does not depend on the desktop it runs on.** On Linux the command line
has its own headless Qt platform that reaches Vulkan through the driver alone.
`export` asks for the new `ApplicationKind::OffscreenDevice`, which selects that
platform whatever `QT_QPA_PLATFORM` says, so an `xcb` session with no `DISPLAY`,
or a stale variable in a shell profile, cannot abort a batch for a reason that
has nothing to do with the photographs. The exception is `--gpu-backend opengl`,
which needs a platform with OpenGL and asks for `Gui`, so it honours
`QT_QPA_PLATFORM`, and so cannot promise `auto`'s fallback: Qt aborts the process
when that platform will not load (see "OpenGL asks for the GPU", below).
`gpu-test` is a diagnostic and still honours the variable always, so someone can
probe xcb Vulkan or OpenGL with it.

**One notice per batch says which device was used**: backend and device name,
silent under `--quiet` and structured under `--log-format json`. A user can tell
whether they got the GPU without timing it.

**Each input is decoded once, and the GPU is blamed only for what the GPU did.**
Decoding (LibRaw, Qt codecs) and `planFor` run on the host before the device
choice, and only `developOnGpu` and `readBack` sit inside the "the GPU could not"
handler. Any exception from those two therefore means the GPU could not,
including `std::invalid_argument` for an image wider than the geometry block can
address exactly. An undecodable file, or settings the plan rejects, fails the
input on either device, with the same message, instead of being retried on the
CPU to fail again and being logged as a GPU fallback. This plans twice, once on
the host and once inside `developOnGpu`, which is cheap; it needed a private
`src/core` include directory on `arraw-cli-core`.

**Devices know when they are lost.** `GpuDevice::isLost()` is true when the lost
flag is set or QRhi reports `isDeviceLost()`, and both `requireUsable` and
`GpuContext::lost` use it. A frame that cannot begin marks the device lost, and
so does one whose `endOffscreenFrame` fails while unwinding, so the loss is
noticed by the frame that suffered it rather than by the next input.

## Added later (2026-09-30): choosing an adapter

**`--device gpuN` is `--device gpu` on adapter N.** A machine can have several
Vulkan devices (an integrated and a discrete GPU, or a GPU and lavapipe), and
`gpu` took whichever one Qt picked. `gpuN` takes the N-th adapter the chosen
`--gpu-backend` lists, counting from 0, in the case-insensitive spelling the
other values have: `gpu0`, `GPU12`. Nothing else about `gpu` changes: it never
falls back, `ARRAW_DISABLE_GPU` is a usage error with it, and a software adapter
still needs `--allow-software`, whichever way it was chosen.

**Numbers are per backend.** Vulkan's `gpu1` and Direct3D 12's `gpu1` need not be
the same device, and the numbering is whatever the backend enumerates, in its
order. `gpu-test` is where a user learns the numbers: it labels each adapter
`gpu0`, `gpu1` and so on. A backend that enumerates nothing (OpenGL, and
possibly Metal) has one device, the default one, which is `gpu0`; any other
number is out of range there.

**An index past the end is a runtime failure, not a usage error.** Whether `gpu3`
exists depends on the machine, so the same command line is well-formed
everywhere and fails only where there is no such adapter, with exit 1 and a
message giving how many adapters the backend lists.

**`gpu-test` tests every adapter by default.** `--device auto` runs each
adapter's report and round trip in its own labelled block and carries on past a
failure, so one broken adapter does not hide the state of the others. `gpu` tests
the default device only, as before, and `gpuN` adapter N alone; `cpu` is a usage
error, since there is no GPU in it to probe. A software adapter that
`--allow-software` did not accept is listed and skipped with a warning
(`Notice::GpuAdapterSkipped`) rather than failed, because in the all-adapters
mode its presence says nothing about the others. The run exits 0 only if at
least one adapter was tested and every one tested passed; if every adapter was
skipped, nothing was tested and it fails, saying so.

**OpenGL asks for the GPU.** `--gpu-backend opengl` with `--device auto` is
`--device gpu`, the one place where the two questions are not kept apart. Its
platform is a display server's, Qt aborts the process when it cannot load, and a
fallback that is promised but cannot be kept is worse than none, so asking for
OpenGL is taken as asking for the GPU. The request is resolved once, when the
arguments are read, and every rule of `gpu` follows: nothing falls back at
creation, an input the GPU fails on fails rather than moving to the CPU, and
`ARRAW_DISABLE_GPU` is a usage error whose message names `--gpu-backend opengl`,
since `--device` was `auto`, not `gpu`. `--device cpu` still wins and ignores the
backend. Every other backend goes through the headless platform and keeps
`auto`'s fallback.

## Consequences

- **The one fallback branch that is untested is device loss mid-batch.**
  `cli::run` creates its own `GpuContext`, so reaching that device would need a
  process-wide hook in production code, and QRhi has no portable way to lose a
  real device. A comment at the branch says so. The rest is covered: per-input
  fallback, undecodable input, and the real-binary platform case.
- **The GPU tests were measured on Vulkan (lavapipe) only.** Parity, including
  NaN and infinity, is unverified on HLSL and MSL back ends, which may compile
  with fast-math. `d3d11`, `d3d12` and `metal` are accepted by the option, not
  vouched for by the tests. The suite can run on them (next section), but the
  tolerances are still lavapipe's.

> **Note (2026-09-30).** On a real GPU (Mesa ANV, Intel HD Graphics 630,
> Vulkan) well-conditioned input still holds the pointwise tolerance, but
> ill-conditioned input does not: negative channels, and a near-zero luminance
> under large channels, reached 3.6e-4 relative against lavapipe's 3e-5,
> because that driver's `pow` is less exact (within Vulkan's precision). The
> tests that probe those branches now hold `illConditionedRelativeTolerance`
> (5e-4), still well short of the 1e-2 a wrong branch gives. At 16 bits export
> differed by up to 26 codes, so export parity is now one code value or that
> relative bound, whichever is more (`tests/gpu/GpuTesting.h`).
- **Wide-image tests write `--format png --overwrite`**, because the same
  `ok.png` appears twice in the batch.
- **No `ARRAW_DEVICE` variable and no config file.** A persistent default is a
  separate decision and can be added beside `--device` without changing it.
- **GPU development stays internal.** `include/` has no device choice, since it
  would put device lifetime and an owner thread into the public contract before
  the scripting work needs it.

## Running the GPU suite on another backend

`arraw-gpu-tests` is built on every platform. `ARRAW_TEST_GPU_BACKEND` picks the
backend, with the names `--gpu-backend` takes, and defaults to the platform's
own; an unknown name fails the run at startup. A backend with no device on the
machine skips every case (exit 4), naming the backend. On Linux the suite uses
arraw's headless platform for every backend except OpenGL, which honours
`QT_QPA_PLATFORM`; Windows and macOS use Qt's native platform.

```
ARRAW_TEST_GPU_BACKEND=opengl QT_QPA_PLATFORM=xcb xvfb-run build/debug/tests/arraw-gpu-tests
```

Windows and macOS now build the suite but it has never been run there, so what
it finds on Direct3D and Metal is unknown.

## What was rejected, and why

**Fall back silently.** The most convenient behaviour and the one ADR 015 forbids
in advance: "it works" and "it worked on the GPU" become indistinguishable.

**Let `auto` use a software rasteriser.** It runs, and it makes CI-shaped
machines look GPU-equipped, but it is usually slower than the CPU chain it
replaces. `--allow-software` exists for the person who wants it, and the tests.

**Fall back for every failure, including a bad file.** It would hide the real
error behind a GPU warning and decode the file a second time. Decode once, plan
on the host, and let only device work count as a device failure.

**Follow `QT_QPA_PLATFORM` for export.** It is right for a diagnostic and wrong
for a batch tool. A platform that cannot start would fail an export that needs
no window at all.
