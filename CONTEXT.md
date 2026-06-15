# arraw

A lightweight, cross-platform RAW photo editor with a Lightroom-style development
workflow: open a folder, edit non-destructively with a real-time GPU preview,
export. Not a DAM, not a catalogue.

## Language

**Culling**:
Marking which frames in the open folder are keepers vs rejects while editing —
a per-file, in-the-moment triage, not a searchable cross-folder catalogue. The
attributes used for culling (rating, colour label) are stored on the file the
same Lightroom-compatible way edits are; there is no index or database.
_Avoid_: cataloguing, library, DAM

**Rating**:
A single per-file culling value on a 0–5 star scale, where 0 is unrated and -1
means rejected — the reject state is folded into the rating, not a separate
pick/reject flag. Persisted as `xmp:Rating` in the sidecar.
_Avoid_: stars (UI term), pick/reject flag (we have no separate flag axis)

**Colour Label**:
A single per-file culling category drawn from a fixed five-colour set — Red,
Yellow, Green, Blue, Purple — or none. Independent of [[Rating]]: a file may
carry any rating and any label at once. Stored as the canonical English colour
name in `xmp:Label` (matching Lightroom's default label set so the colours
survive a round-trip). The meaning of each colour is the user's own convention.
_Avoid_: tag, keyword, free-text label

**User Metadata**:
Writable, user-authored metadata that travels with an image — today the
[[Rating]] and [[Colour Label]], plausibly caption/keywords later. Persisted as
XMP (`xmp:`/`dc:`) alongside the develop settings. Distinct from the read-only
camera EXIF shown in the Exif panel, which the user never edits.
_Avoid_: image metadata (that name is the read-only EXIF rows), tags, catalogue data

**Working color space**:
The color space all pixels live in from RAW decode until the final display/output
transform: linear-light Rec.2020 primaries. Every adjustment operates in this space.
_Avoid_: internal color space, pipeline color space

**Display transform**:
The conversion from working color space to what the monitor expects, applied as the
last step of preview rendering.
_Avoid_: gamma step, sRGB step

**Output transform**:
The conversion from working color space to the color profile chosen for an exported
file (sRGB, Display P3, AdobeRGB).
_Avoid_: export color conversion

**Soft-proofing**:
A preview mode that temporarily replaces the display transform with a simulation of
a printer/paper profile, so the screen predicts the print.
_Avoid_: print preview, proof mode

**Tone Curve**:
A user-editable remapping of gamma-encoded (display-space) values, defined by
control points in [0,1]². One Luma Curve plus three Channel Curves per image.
_Avoid_: curves (ambiguous with the LUT that implements them)

**Luma Curve**:
The tone curve applied to luminance; it scales RGB proportionally so hue is
preserved.
_Avoid_: master curve, RGB curve

**Channel Curve**:
A tone curve applied to a single R, G, or B channel independently; can shift
hue by design.

**Curve Input Histogram**:
The histogram of the image as the tone curve receives it — after upstream
adjustments, gamma-encoded — drawn behind the curve so its x-axis matches the
curve's. Distinct from the panel Histogram, which shows the final image.

**Local Adjustment**:
A develop edit that applies only within a masked region of one image — a
*([[Mask]] + tonal/colour deltas)* pair, as opposed to the global adjustments
that touch every pixel identically. Carries the dodge/burn + colour-grade subset
(exposure, contrast, highlights, shadows, whites, blacks, temperature, tint,
saturation, vibrance), never geometry or the tone curve. Previews live, lives on
the develop undo stack, and is stored in arraw's own XMP namespace (not `crs:`).
_Avoid_: filter, layer, adjustment brush (the brush is one possible future mask
source, not the concept itself)

**Mask**:
The stencil that says *where* a [[Local Adjustment]] applies — value 0 (none) to
1 (full), grey in between. In v1 it is *described* parametrically (a shape or a
value range), not painted; a freehand brush is a documented later extension.
_Avoid_: selection, channel, cut-out

**Mask Type**:
The kind of parametric [[Mask]]: Linear (a graduated fade across the frame) and
Radial (an oval) in v1; Luminance range and Colour range are the planned next
set. The brush, if added, becomes a further type.

**Develop Preset**:
A saved, named bundle of develop settings, applied to any photo through the same
"pick which groups travel" list used by copy/paste. Local Adjustments may be
included but are off by default (a mask pinned to one photo's geometry rarely
generalises). Stored as arraw-native files in the app data directory, not as a
develop sidecar and not Lightroom-compatible.
_Avoid_: profile (that is a colour or camera profile), template, style

**Straighten**:
Leveling the image by drawing a reference line along something that should be
horizontal or vertical; the line's angle is written to Rotation. The gesture,
not the stored value (that is Rotation).
_Avoid_: deskew, level, auto-rotate

**Rotation**:
The signed angle (±45°) applied to the image, exposed as a slider and also set
by Straighten. The stored value, not the gesture.
