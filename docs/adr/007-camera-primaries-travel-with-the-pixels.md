# Camera primaries travel with the pixels, and white balance ends the camera space

This ADR is written ahead of the code it describes, to settle the shape develop
settings are built against. Nothing below is implemented yet.

`rawimport::load` hands back a *neutral development*: LibRaw demosaics, applies
the camera's as-shot multipliers, and converts through the camera's matrix into
linear Rec.2020. ADR 005 recorded that as a deliberate simplification and named
a future `RawLoadOptions` as the place it would be reopened. White balance is
the first develop setting that reopens it, because a per-channel gain is only a
white balance in the *camera's* space. Once the pixels have been through the
camera matrix, the channels are Rec.2020 primaries and scaling them is a tint,
not a white balance.

The previous implementation did exactly that — `main`'s `WhiteBalance` applies
a blackbody-derived gain in linear Rec.2020, normalised so 5500 K returns
`{1,1,1}` (`src/develop/WhiteBalance.h`, ADR 0025 on `main`). Its Kelvin
numbers are therefore a dial rather than a measurement: a tungsten frame and a
daylight frame both open at 5500 K, and the value is stored as
`arraw:Temperature` rather than `crs:Temperature` precisely because it does not
mean what Lightroom's means.

Two further requirements point the same way. The demosaic algorithm is to
become a develop setting, which means working from the sensor mosaic rather
than from LibRaw's finished RGB. And lateral chromatic aberration is a
per-channel geometric shift of the *sensor's* channels: after the matrix, each
channel is a mixture and per-channel geometry stops meaning anything.

## Decision

**RAW decode stops converting to the working space.** LibRaw's `output_color`
becomes 0, and the decoded buffer carries the camera's own primaries.

**`ColorEncoding` becomes a variant**, because a camera's primaries are
per-body data that no enumerator can name:

```cpp
enum class NamedEncoding { LinearRec2020, Srgb, DisplayP3, AdobeRgb };

using Gains = std::array<float, 3>;

struct CameraNative {
    Matrix3 toWorking;          ///< Camera RGB to linear Rec.2020.
    Gains daylightScale{};      ///< Row scales LibRaw normalised out of toWorking.
    Gains asShotMultipliers{};  ///< Gains the camera recorded, green == 1.
    Gains appliedMultipliers{}; ///< Gains the decode actually applied.
};

using ColorEncoding = std::variant<NamedEncoding, CameraNative>;
```

The description travels with the pixels rather than beside them. A buffer that
claimed to be camera-native without its matrix would be the partially populated
state ADR 001 exists to prevent, and nothing downstream can recover the matrix
from the samples.

**The matrix is `imgdata.color.rgb_cam` composed with a fixed sRGB to Rec.2020
matrix — but `rgb_cam` alone is not enough.** Measured against LibRaw 0.22.2:
`rgb_cam`, `cmatrix`, `cam_mul` and `pre_mul` are all populated by `open_file`,
before `unpack`, so owning the demosaic costs nothing here. `cam_xyz` is *not* a
safe source — it is filled from LibRaw's built-in table keyed on make and model,
and reads as all zeros for any body the table does not know, our own test
fixtures included. `dng_color[i].colormatrix` and `forwardmatrix` are left for
the day two-illuminant interpolation by correlated colour temperature is wanted;
`rgb_cam` is LibRaw's own resolution of that precedence and always has an
answer.

What `rgb_cam` has lost is the scale of each camera-response row.
`cam_xyz_coeff` normalises the camera matrix so that `cam_rgb * (1,1,1)` is
`(1,1,1)` before inverting it, and stores the divisors it used in `pre_mul`.
Measured on two DNGs from this repository's own fixture writer, identical except
that the second doubles the first row of `ColorMatrix1`:

| After `open_file` | baseline | doubled row |
|---|---|---|
| `cam_mul` | 2.0000 1.0000 1.2500 | 2.0000 1.0000 1.2500 |
| `rgb_cam[0]` | 1.0000 -0.0000 0.0000 | 1.0000 -0.0000 0.0000 |
| `pre_mul` | **1.0000** 0.9999 1.0002 | **0.5000** 0.9999 1.0002 |

Two different sensor calibrations that `rgb_cam` and `cam_mul` cannot tell
apart. A Kelvin-to-gains solve built on them alone would treat every sensor as
already balanced and return the same answer for both, which would break both
as-shot Kelvin recovery and copying an absolute temperature between bodies. So
`pre_mul` is stored too, as `daylightScale`, and the unnormalised camera matrix
is reconstructed from the pair.

**`pre_mul` is captured after `open_file` and before any processing**, because
`scale_colors` overwrites it. Measured on the same file: `0.5000 0.9999 1.0002`
at open, `1.0000 1.9999 2.0003 1.9999` after `dcraw_process`. Read late, it
means something else entirely.

**Recorded gains and applied gains are stored separately.** `asShotMultipliers`
is what the camera wrote, in LibRaw's `cam_mul` convention — per-channel
*multipliers* normalised so green is 1, not the DNG `AsShotNeutral` convention
of neutral channel *values*, which is its reciprocal. `appliedMultipliers` is
what the decode actually used, and the two differ for a file that declares no
neutral, where ADR 005 substitutes the daylight multipliers the colour matrix
implies. Anything computing a temperature from the buffer needs the applied
ones; anything reporting what the camera saw needs the recorded ones.

**Camera space is a region of the pipeline, not a transient:**

```
mosaic -> demosaic -> CA -> (distortion, vignetting) -> white balance + matrix -> working space -> tone, colour, ...
                      \____________ camera space ____________/
```

Chromatic aberration correction precedes the matrix for the reason above.
Distortion and vignetting commute with it exactly — resampling is linear in the
sample values, vignetting is a per-pixel scalar, and the matrix is per-pixel
across channels — so they may sit on either side.

**One raster type, two spaces.** Spot removal and lens corrections take and
return an `ImageBuffer`, preserving whatever encoding they were given, so a
single implementation serves a RAW mid-pipeline and a JPEG that never had a
camera space at all. Giving camera space its own raster type would fork every
geometric and retouch algorithm in two. The sensor mosaic *is* a distinct type
(`RawMosaic`: one plane, a CFA pattern, black and white levels, and the
`CameraNative` value that carries on to the demosaiced buffer), because it is
not a colour image — as ADR 001 already says.

**Export refuses camera-native buffers.** `ExportOptions::encoding` narrows to
`NamedEncoding`, so "export to camera native" stops being expressible, and
`exportImage` rejects a camera-native *source* with a message that says which
stage is missing rather than "unknown colour encoding".

**The entry point is a free function**, `develop(source, settings, request)`,
returning a working-space buffer. The request describes what the caller wants
rendered and defaults to the whole frame at full size:

```cpp
struct RenderRequest {
    std::optional<ImageSize> targetSize; ///< Fitted inside, after crop.
    Upscale upscale = Upscale::Never;    ///< Never, or Allowed.
};
```

The upscale policy is part of the request rather than the caller's arithmetic,
because resolving a requested size needs the post-crop dimensions, and only the
pipeline knows those. A caller that clamped for itself would be reconstructing
the geometry chain outside the library — which is how the command line and the
interface drift apart.

It is there from the start because resizing has two callers, not one. The
feature brief promises "re-exporting a shoot in a different size or profile
without touching the edits", and a GUI cannot run a 60 MP frame through the
pipeline per slider tick. Both are the same parameter.

Resizing belongs here rather than on `ExportOptions`, for three reasons. Grain,
sharpening and noise reduction mean different things at different output
scales, so resampling a finished buffer applies them at the wrong one. Two ways
to resize is the shape of the defect the reimplementation plan documents on
`main`, where batch export assembled its own sequence and silently dropped lens
corrections. And a preview and an export that are the same call cannot drift,
which is what the brief means by the preview, the export and the command line
running the same processing. `exportImage` keeps doing one thing: encoding the
buffer it is given.

**The command line says "smaller" in one flag with three forms.** `--resize
2048` is the long edge, `--resize 2048x1365` fits inside a box, and `--resize
50%` scales. Each form is recognisable from its own shape, so there is no mode
flag and no ordering question. Long edge rather than width, because it is the
only form that treats a portrait and a landscape frame alike in a batch. More
forms — megapixels, short edge — can join later provided each has a syntax of
its own, so that no existing form changes meaning: a bare number is the long
edge permanently.

**Resizing only ever shrinks.** A 2048 long edge asked of a 1600 pixel frame
leaves it at 1600 rather than interpolating upwards, unless `--allow-upscale`
says otherwise. The size applies to the photograph after its crop, per ADR 009's
composition frame.

Where the pipeline resamples is its own business, not the caller's — full size
for an export, an early downsample for a preview — and that difference is the
quality policy the plan's §9 names. `targetSize` is implemented when a
`--resize` flag exists; region and quality join the same struct later without
churning the signature. There is no state to own yet: nothing is cached and
there is no GPU. It becomes a processor object at the first cache — ADR 001
already puts decoded and derived buffers in a processor's hands — and moves
behind `Photo` and an editing session at the first sidecar. The signature is
what a `render` member would have, so both promotions are mechanical.

**Development happens in `RgbaF32`, named by a constant.** Exposure pushes
samples above 1, white-balance gains do the same, and the camera matrix
produces genuine negatives for sensor colours outside Rec.2020 — none of which
`RgbaU16` can hold, and all of which are exactly what the wide working space
exists to keep. `develop` widens once on entry and returns float; `exportImage`
narrows to eight or sixteen bits as it already does. Following ADR 003's
pattern, `PixelFormat::RgbaF32` names the layout and
`arraw::workingFormat` names the role it plays, so every stage allocates the
constant and changing it is a one-line change.

**The alpha channel passes through development untouched.** No sensor records
one and no develop setting produces transparency: masks are their own rasters
and a crop removes pixels rather than hiding them. But a PNG or TIFF input can
carry straight alpha, and preserving it is already a tested property
(`test_ImageExport.cpp`, "16-bit exports preserve precision and straight
alpha"). So the matrix applies to RGB, exposure and white balance scale RGB,
and the fourth channel is copied — padding for a RAW, data for anything else.

## Consequences

- **`loadImage`'s signature does not change.** Buffers self-describe, so a RAW
  comes back camera-native and a JPEG working, both as `ImageBuffer`.
- **Existing tests keep their meaning with a one-call insertion.**
  `develop(buffer, {})` is identity for a JPEG and the matrix alone for a RAW,
  so `exportImage(develop(loadImage(f), {}), ...)` still round-trips.
- **The matrix path is untested until the fixtures can see it.** Every fixture
  today resolves to an identity `ColorMatrix1` and a unity daylight scale, so
  the matrix code could be deleted and the suite would pass. Establishing this
  contract needs four things the current set cannot show: a skewed
  `ColorMatrix1`, a non-unity daylight scale, a non-unity as-shot neutral, and a
  file declaring no neutral at all. All are constants in
  `make_raw_fixtures.py`, but they have to be a conscious addition, together
  with a temperature/tint -> gains -> temperature/tint round trip.
- **Clipped highlights stay clipped.** `params.highlight = 0` clips at the
  camera white level before we see the pixels, so a white balance that scales a
  clipped channel gives coloured highlights. Only making the decode itself a
  develop stage can fix that; it is not fixed here.
- **Our colours will not match Lightroom's at identical settings.** A single
  matrix is not a DNG camera profile, which also carries a forward matrix and a
  hue/saturation lookup. What round-trips is the *setting*, not the rendering.
- **The demosaic uses the as-shot multipliers, always.** Demosaicing wants
  white-balanced data, but feeding the user's Temperature into it would make
  every slider tick invalidate the most expensive cache in the program. The
  user's delta is applied in the matrix instead. The cost is a slightly
  sub-optimal demosaic at extreme white balance settings.
- Five test sites change `ColorEncoding::LinearRec2020` to
  `NamedEncoding::LinearRec2020`, and `toColorSpace`'s switch becomes
  exhaustive, losing its trailing `throw`.

**Sixteen-bit floats would be worth having, later.** `RgbaF16` halves the
working set against `RgbaF32` — 480 MB rather than 960 MB for a 60 MP frame —
and is natively supported by both ends we care about: `QRhiTexture::RGBA16F`
and `QImage::Format_RGBA16FPx4`. It is also what OpenEXR uses for scene-linear
data, so the precision question has a well-tested answer. It is deferred rather
than rejected, for two reasons. The saving is only worth measuring once caches
and GPU upload exist, and the sample *type* is not portable today: verified,
`std::float16_t` is available on GCC and libstdc++ 13+ but not in libc++ (so
not on macOS) and not in MSVC, whose `<stdfloat>` is deliberately empty. When
the enumerator lands, the choice is `qfloat16` — portable, but it puts a QtCore
header in a public header that is currently pure `std` — or a forty-line `Half`
of our own that can be re-pointed at `std::float16_t` later. That is a decision
to take against a real Windows build, not against an assumption.

Dropping alpha to save 25% was considered and rejected: `QRhiTexture::Format`
has no three-component format at all, so a three-channel buffer would be
repacked on every GPU upload and stored as four channels regardless, and a
16-byte RGBA float pixel is one SIMD register while a 12-byte one is not.

