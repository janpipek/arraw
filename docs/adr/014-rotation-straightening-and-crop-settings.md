# Rotation and straighten preserve framing; crop remembers its constraint

This decision defines public values and their CPU rendering contract. Camera
orientation extraction, geometry resolution and rendering are implemented, and
CLI flags apply them; see the README for syntax. Interactive editing commands,
copying between photographs, serialization and GUI controls remain future work.

## Decision

**The first geometry settings are quarter-turns, flips, straighten and crop.**
Perspective, guides, lens correction and output sizing are outside this slice.
`GeometrySettings` is a plain comparable aggregate inside `DevelopSettings`;
`GeometrySettings.h` supplies its public value types. There are no matrices,
rendering functions or GUI state in these values.

**Camera orientation is always honoured.** It belongs to source metadata,
including mirrored orientations; missing orientation means identity. User
rotation and flips are relative to this baseline, so resetting geometry restores
the camera's intended orientation, not necessarily the decoded buffer's axes.
The settings do not provide an ignore-orientation override. `ImageMetadata` and
decoded `ImageBuffer` values carry an `ImageOrientation`, using EXIF's eight
orientation values. Import leaves the raster unrotated, clones retain the tag,
and development returns physically oriented pixels tagged Normal. Qt automatic
orientation is explicitly disabled during decode. LibRaw's flip codes and
[Qt's transformation flags](https://doc.qt.io/qt-6/qimageiohandler.html#Transformation-enum)
are converted to this common representation at import.

**The order is camera orientation, straighten, quarter-turn, horizontal flip,
vertical flip, crop.** Future upstream lens correction keeps ADR 009's anchors.
Straighten rotates about the full image centre, with clockwise positive and an
inclusive range of -45 to +45 degrees. Zero is neutral. Quarter-turns are an enum
with four values, not an unrestricted angle. Flips refer to the final upright
axes. Reflection reverses the apparent direction of an earlier rotation: after
one flip, increasing the stored straighten angle appears counterclockwise.
An eventual displayed-angle control must account for this rather than changing
the stored transform order.

The two flip booleans intentionally allow equivalent representations (both
flips equal a half-turn). Equality compares stored editing state, not geometric
equivalence. A future rotate/flip command must compose the requested operation
in displayed axes; changing an enum alone is not sufficient in every mirrored
state.

**A crop is optional, and absence means automatic framing.** With no explicit
rectangle, choose the largest-area axis-aligned rectangle inside valid content
that satisfies the aspect constraint. Free aspect permits any ratio. Break
equal-area ties by nearest image centre, then smallest top and left edges.
At neutral geometry, free automatic framing is the full image.

An explicit full-frame rectangle is distinct from absence: it represents a
user selection and follows the preservation rule below. Resetting the crop
rectangle returns to automatic framing while retaining its aspect constraint;
resetting all crop settings also returns the aspect to free. Resetting all
geometry restores every field's default.

**Crops always stay inside valid image content.** There is no allow-empty-corners
switch or canvas background setting. A rotated image's bounding rectangle
contains empty wedges; checking only normalised bounds is insufficient.
The geometry resolver checks the actual valid region.

**The storage frame is the uncropped upright bounding rectangle.** After all
orientation and straighten operations, translate the continuous bounding box
to a top-left origin. Normalise horizontal coordinates by its width and
vertical coordinates by its height. The edges run from zero to one, x rightward
and y downward. They describe image boundaries, not pixel centres; a source
pixel centre is at `(i + 0.5, j + 0.5)` in source edge units. These dimensions
precede raster rounding, so preview resolution cannot change the stored crop.

`UprightCropRect` is a distinct frame-specific type, as ADR 009 requires.
Its edges are doubles, with strictly positive width and height. A physical
aspect is `(right - left) * uprightWidth / ((bottom - top) * uprightHeight)`;
normalised width divided by normalised height is not the physical aspect.
Source geometry uses the decoded visible frame; camera-recommended default
crop metadata is not introduced as another crop layer in this slice.

**Aspect is remembered per photograph, independently of the rectangle.**
`CropAspect` is one of free, original, or a positive finite custom
width-over-height ratio. A variant avoids meaningless custom values in the
other modes. Named presets such as 3:2 are presentation of a custom ratio, not
additional model modes. Portrait and landscape are represented by reciprocal
ratios; square is 1. Original uses the full image after camera orientation and
user quarter-turns, before straighten and crop. An absent rectangle may still
have a locked aspect: automatic framing then finds the largest fit at that
ratio. The default is free.

**Quarter-turns and flips carry the explicit crop with the selected content.**
As in ADR 009, remap its edges by swaps and complements rather than refitting
it. For a clockwise quarter-turn, `(left, top, right, bottom)` becomes
`(1 - bottom, left, 1 - top, right)`. A horizontal flip becomes
`(1 - right, top, 1 - left, bottom)`; a vertical flip is analogous.
A quarter-turn also reciprocates a custom aspect; original resolves to the
new orientation's aspect. Flips leave aspect unchanged. Free remains free.
Absence remains absence. These operations are exact geometrically, though
floating-point complements need not be bitwise reversible for every input;
future tests must use appropriate numeric tolerances.

**Changing straighten preserves an explicit crop instead of starting over.**
Carry its centre offset from the full image centre into the new upright bounds
without rotating that offset, and retain its physical width and height as the
initial candidate. The image rotates beneath the selection. This preserves
framing rather than exactly the same selected pixels, which an arbitrary
rotation cannot do with an axis-aligned rectangle.

If that candidate does not fit, shrink uniformly around its centre, retaining
its current physical aspect even in free mode. If no positive-area rectangle
can fit at that centre, move the centre to admit the candidate, shrinking if
required. Fit choices use largest retained scale
(never above 1) at the retained centre first; when moving is necessary, prefer
the largest retained scale, then the nearest feasible centre, then top/left.
An already valid crop is untouched. A crop that was automatically framed
remains automatic and resolves afresh. Intermediate drag updates should start
from the gesture's initial selection to avoid cumulative shrinking; undo holds
the previous settings snapshot. Rendering must not mutate document settings.

**Copying crop is explicit opt-in for both presets and copy/paste.** Rotation,
flips and straighten can be selected separately; crop's rectangle and aspect
travel together. An excluded crop leaves both destination values unchanged.
An automatic crop transfers as automatic with its constraint. Original remains
relative to the destination; a custom ratio retains its literal value.

For an explicit crop, transfer the normalised centre and its physical long-edge
length divided by the source upright frame's long-edge length. Reconstruct on
the destination using its long edge. Preserve the source crop's physical aspect
for free/custom modes; original instead resolves against the destination.
Shrink uniformly or shift only as needed using the fitting policy above. Thus
a free square crop stays square even between landscape and portrait images.
This operation needs both photographs' resolved frame dimensions: copying the
four stored edges is not enough. A portable preset containing an explicit crop
must carry the source aspect and relative size as transfer data, not pretend
that `DevelopSettings` alone supplies the missing dimensions.

**Validation happens before pixel processing.**
Straighten and all coordinates must be finite; straighten is within its named
limits, ratios are positive and finite, enum values are recognised, and edges
satisfy `0 <= left < right <= 1` and `0 <= top < bottom <= 1`.
Actual-content containment and aspect agreement require resolved geometry.
Nonfinite, unordered or out-of-range direct input is rejected per ADR 008.
Explicit rectangles must agree with a locked aspect to a relative tolerance
of 1e-6. A finite out-of-range straighten
loaded from storage may be clamped with a warning. An unusable crop (nonfinite,
empty or inverted) falls back to automatic framing with a warning; an invalid
aspect falls back to free. A usable rectangle that falls outside valid content
is fitted with a warning. Those storage-load recovery rules remain future work;
the current resolver fits a structurally valid requested rectangle into valid
content without mutating settings. No sidecar reader is implemented here.

## CPU rendering contract

`GeometryPlan` holds the source dimensions, an orthogonal source-to-upright
matrix, continuous upright dimensions, fitted crop and output raster dimensions.
It supplies distinct source/upright point types and forward/inverse mappings.
Plans resolved from a buffer or `Photo` include geometry in value equality;
the encoding-only planning overload still resolves just colour and tone.

Camera orientation, quarter-turns and flips use exact integer coefficients.
Straighten introduces sine and cosine once per plan. Every geometric operation
is composed before sampling. Without lens correction the valid region is a
rotated rectangle, so crop containment is equivalent to bounding its four
corners in the source frame. Symmetry puts an automatic maximum-area crop at
the image centre; its dimensions follow from the two source-axis constraints.
An explicit crop retains its centre and physical aspect while shrinking. If
the centre admits no positive-area crop, the largest retained size is chosen
and its centre projected to the nearest feasible point in source axes.

Colour and tone are evaluated over the decoded raster first. The geometry pass
then maps output pixel centres back into this linear working-colour buffer.
It uses bilinear interpolation with premultiplied alpha and converts back to
straight alpha; transparent coloured neighbours therefore create no fringes.
The half-pixel strips beyond the outermost source pixel centres use edge
extension, within the already validated image boundary. Exact pixel centres
copy all four channels, including RGB under zero alpha. Numerical noise within
32 double-precision epsilons times the source-axis length snaps to an exact
pixel centre. Identity geometry reuses the developed buffer without another
allocation.

Raster width and height are the floors of continuous crop extents, with a
minimum of one pixel per axis. Extents within relative 1e-10 of an integer
are rounded to that integer first. The continuous crop stays unchanged;
rounding affects sampling density rather than selecting different content.
Small aspect deviations in raster dimensions are unavoidable for odd dimensions
and very small crops. Crop coordinates and aspect constraints remain continuous.
There is no separate preview geometry path. Requested target-size resizing,
including its antialiasing policy, remains outside this implementation.

## Consequences

- Settings resolve once into geometry shared by sampling and coordinate mapping.
- The future geometry editor owns dependent crop/aspect updates. Plain field
  assignment stores a new value; it does not remap an existing crop from an old
  frame. The CLI describes one final geometry state, not a sequence of edits.
- Tests cover mirrored camera orientations, off-centre crops, non-square and
  odd-sized images, locked ratios, forward/inverse mapping, exact remapping,
  fractional sampling and alpha. Preview sizing and copying between unlike
  proportions need tests when those operations are implemented.
- ADR 008's future descriptors will enumerate geometry leaves and their
  constraints. Geometry applies to RAW and non-RAW photographs alike; sidecar
  mapping and interoperability remain separate work.
