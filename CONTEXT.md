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

**Shot**:
A single capture as the filmstrip presents it: one cell, even when the camera
wrote several files sharing a base name (RAW + JPEG). The RAW is the primary —
the file shown and edited — and same-stem standard images are its companions.
Files that do not pair stand alone as their own shot.
_Avoid_: group (overloaded with [[Develop Group]]), frame (a shot may be several files)

**Format Label**:
The chip on a filmstrip cell naming the image formats present in that [[Shot]],
primary first — "ARW", "JPEG", or "ARW+JPEG" when a RAW carries companions.
Always shown on every cell, one token per distinct canonical format name
(jpg/jpeg → JPEG, tif/tiff → TIFF, RAW types as their extension). It describes
the frame's own formats; it is not [[User Metadata]].
_Avoid_: companion badge (the old name — it advertised only the hidden companion),
extension, file type

**User Metadata**:
Writable, user-authored metadata that travels with an image — today the
[[Rating]] and [[Colour Label]], plausibly caption/keywords later. Persisted as
XMP (`xmp:`/`dc:`) alongside the develop settings. Distinct from the read-only
camera EXIF shown in the Exif panel, which the user never edits.
_Avoid_: image metadata (that name is the read-only EXIF rows), tags, catalogue data

**XMP Property Ownership**:
The rule for which shared-sidecar properties arraw may replace: the complete
`arraw:` namespace, its modeled develop properties in `crs:`, and Rating plus
Colour Label in `xmp:`. Every other property belongs to the wider XMP ecosystem
and must survive arraw saves semantically.
_Avoid_: namespace ownership (`crs:` and `xmp:` are shared), digiKam metadata
(the rule applies to every XMP editor)

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

**Clipping**:
A pixel where at least one channel hits the limit of the sRGB display range —
≥ 1.0 is highlight clipping (detail lost to white), ≤ 0.0 is shadow clipping
(detail lost to black). Judged at the [[Display transform]], sRGB-relative,
the same way regardless of [[Soft-proofing]]. A tonal concern, distinct from
the [[Gamut Warning]], which is about chroma.
_Avoid_: extremes (the branch name, not the domain term), blown/crushed
(informal), gamut warning

**Clipping Overlay**:
The preview mode that paints clipped pixels onto the image — highlight
[[Clipping]] in red, shadow clipping in blue — so lost detail is visible while
developing. Highlight red wins where a pixel clips both ways, and over the
[[Gamut Warning]] red. Highlights and shadows are two independent toggles
(`J` flips both at once); view state in QSettings, never in the sidecar.
_Avoid_: show extremes, zebra, clipping mask

**Gamut Warning**:
A [[Soft-proofing]]-only overlay (red) marking pixels whose colour falls
outside the proofed output profile's gamut — a chroma-reproduction warning,
not a tonal one. Distinct from [[Clipping]].
_Avoid_: clipping, out-of-range

**White Balance**:
Neutralising an unwanted colour cast by scaling each colour channel by its own
gain in the [[Working color space]] — the channels are *multiplied*, never
offset, so a pixel carrying no light (black) keeps carrying none and can never
acquire colour. Two controls: **Temperature**, the warm↔cool axis named in
Kelvin (lower = warmer, higher = cooler), and **Tint**, the orthogonal
green↔magenta axis. Neutral (5500 K, tint 0) leaves the image untouched.
Available globally and, as a relative nudge, per [[Local Adjustment]].
_Avoid_: colour balance (informal slider name), additive shift, colour cast
(that is the defect White Balance removes, not the control itself)

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

**Exposure**:
A broad, stop-like brightness adjustment concentrated through the perceptual
midtones; black and white stay anchored. _Avoid_: scene-linear gain

**Contrast**:
Expansion or contraction of tones around perceptual middle grey while black and
white stay anchored. _Avoid_: endpoint clipping

**Shadows / Highlights**:
Regional tone controls that separate or compress dark/bright detail while
preserving the black/white endpoints. _Avoid_: black point, white point

**Blacks / Whites**:
The two clipping-point controls: Blacks deliberately changes shadow clipping and
Whites deliberately changes highlight clipping. _Avoid_: Shadows, Highlights

**Recoverable Headroom**:
Developed working values above display white (1.0) that remain available to later
adjustments and are clipped only by the display or a bounded output encoding.
_Avoid_: already clipped, invalid values

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

**Develop Group**:
One selectable unit in the [[Copy Settings]] / [[Develop Preset]] checklist —
the granularity at which develop settings travel between photos. The nine
groups partition every global field: White Balance, Tone, Tone Curve, Colour,
HSL, Detail, Geometry ([[Rotation]] + [[Crop]] + [[Aspect Ratio Lock]] together),
[[Lens Corrections]] ([[Distortion]] + corrective [[Vignetting]] + [[Chromatic
Aberration]]), and Effects ([[Post-Crop Vignette]] + [[Grain]]). Per-image state such as a Grain's hidden
seed and [[Local Adjustment]] masks does not travel with a group. Applying a
group **replaces** every visible field in it on the target, including resetting
to defaults when the source group is unedited.
_Avoid_: section, category, panel

**Copy Settings**:
Snapshotting the open photo's chosen [[Develop Group]]s into an in-memory
clipboard (one slot, session-only, never the OS clipboard). Copy opens the
group checklist; [[Paste Settings]] later applies them. There is no implicit
"copy from the previous photo" gesture — propagation is always explicit.
_Avoid_: yank, duplicate, sync

**Paste Settings**:
Applying the [[Copy Settings]] clipboard onto the open photo as a single undo
step. Paste opens its own checklist bounded by what was copied — you may narrow
(uncheck) but never paste a [[Develop Group]] that was not copied.
_Avoid_: sync, merge (paste replaces each group, it does not merge)

**Develop Preset**:
A saved, named bundle of selected [[Develop Group]]s, applied to any photo in
one click (no apply-time checklist — narrowing happens only at save). The file
is **partial**: it stores exactly the groups chosen at save, so its contents are
the groups it sets and nothing else. [[Local Adjustment]]s are never included.
Stored as one arraw-native JSON file per preset in the app data directory — not
a develop sidecar, not Lightroom-compatible.
_Avoid_: profile (that is a colour or camera profile), template, style

**Straighten**:
Leveling the image by drawing a reference line along something that should be
horizontal or vertical; the line's angle is written to Rotation. The gesture,
not the stored value (that is Rotation).
_Avoid_: deskew, level, auto-rotate

**Rotation**:
The signed *fine* angle (±45°) applied to the image to level it, exposed as a
slider and also set by [[Straighten]]. The continuous tilt only — coarse 90°
turns and mirroring are a separate concept (see Coarse Orientation below), so
this value never leaves the ±45° band. The stored value, not the gesture.

**Orientation**:
The *coarse*, discrete framing of the image: one of four 90° quarter-turns
(0/90/180/270) optionally combined with a horizontal or vertical mirror — the
eight states the EXIF Orientation tag encodes. Distinct from [[Rotation]] (the
continuous ±45° level): Orientation is "which way up / which way round the frame
sits," set by the camera at capture and by the Rotate 90° and [[Flip]] commands;
Rotation is the fine tilt on top of it. Lossless and exact (no resampling — a
90° turn swaps the axes, a mirror negates one). Seeds from the camera's EXIF tag
on load so a portrait shot opens upright. Not to be confused with the crop tool's
[[Flip Aspect]], which orients the *crop ratio*, not the image.
_Avoid_: rotation (that is the fine ±45° angle), flip (one operation, not the state)

**Flip**:
Mirroring the image horizontally or vertically — a state of [[Orientation]], not
its own axis. Distinct from [[Flip Aspect]] (which swaps the crop ratio's
width:height and never touches pixels).
_Avoid_: mirror (UI may say "Flip"), reflect

**Crop**:
The axis-aligned rectangular region of the rotated display frame that is kept;
everything outside it is discarded from the developed and exported image.
Defined in the display frame so it survives [[Rotation]] (crop after rotation),
and stored as normalised edges. The stored value (the rectangle), as distinct
from the gesture of dragging its handles.
_Avoid_: trim, frame, mask ([[Mask]] is the local-adjustment stencil)

**Aspect Ratio Lock**:
A crop-tool state that constrains the [[Crop]] rectangle to a fixed width:height
while dragging — Free (unconstrained), Original (the image's own proportions), or
a preset (1:1, 2:3, 3:4, 4:5, 16:9), with a [[Flip Aspect]] toggle that swaps
width and height. *Whether* a crop is locked is persisted, as the Lightroom-compatible
`crs:CropConstrainAspectRatio` flag; on reload the lock re-engages at the stored
rectangle's ratio. The exact preset is **not** stored — the ratio lives in the
rectangle, and the preset name is re-derived on load (a ratio matching no preset,
e.g. from a foreign editor, round-trips as a nameless "locked" state). Free is the
absence of the flag.
_Avoid_: constrain, fix ratio, crop preset

**Flip Aspect**:
The crop-tool toggle that swaps the [[Aspect Ratio Lock]]'s target width:height
(landscape ↔ portrait). Reshapes the crop rectangle only; never touches image
pixels or [[Orientation]]. Was historically mislabelled "Flip Orientation".
_Avoid_: flip orientation (collides with image [[Orientation]]), rotate crop

**Post-Crop Vignette**:
A global develop *effect* that darkens or lightens the outside of the final
[[Crop]] with a centred elliptical falloff, for creative emphasis. Its Amount,
Midpoint, and Feather travel together in the Effects [[Develop Group]] and are
stored as `crs:PostCropVignette*`. Distinct from corrective [[Vignetting]], which
*removes* a lens's light falloff in sensor space; this one *adds* falloff for
look, post-crop.
_Avoid_: Vignette (bare — collides with corrective [[Vignetting]]), edge burn,
radial mask (a [[Mask]] is a Local Adjustment stencil)

**Lens Corrections**:
The [[Develop Group]] that removes a lens's optical defects using a [[Lens
Profile]], so the developed image reflects the scene rather than the glass.
Holds three profile-driven corrections — [[Distortion]], corrective
[[Vignetting]], and [[Chromatic Aberration]] — each an independent on/off toggle.
Not creative controls and not sliders the user drags: the strength comes from the
profile. Applied CPU-side to the decoded [[ImageBuffer]] before [[Spot]]s and the
shader (the corrected negative), so everything downstream develops on corrected
pixels.
_Avoid_: lens profile (that is the data source, [[Lens Profile]]), optics, profile
corrections

**Lens Profile**:
The data that describes one lens's defects at the shot's focal length and
aperture — the distortion polynomial, vignette falloff, and per-channel
chromatic scale that drive [[Lens Corrections]]. Sourced either from data
**embedded** in the RAW (DNG opcodes or maker notes) or from the external
**lensfun** database, matched via the lens identity in the EXIF. The sidecar
records *which* profile and source were used plus the per-correction toggles; the
coefficients are re-derived on load, never stored.
_Avoid_: lens database, calibration, [[Develop Preset]] (unrelated)

**Distortion**:
The [[Lens Corrections]] component that straightens barrel/pincushion geometry by
resampling pixels along the profile's radial polynomial. Because it moves pixels,
the corrected frame is auto-scaled to fill and the default [[Crop]] is refit to
the largest clean rectangle (as with [[Rotation]]).
_Avoid_: warp, geometry (that is the [[Crop]]/[[Rotation]] group), perspective

**Vignetting**:
The corrective [[Lens Corrections]] component that *removes* a lens's corner
light falloff by applying the profile's radial gain — the inverse of what
[[Post-Crop Vignette]] adds. Stored as `crs:VignetteAmount` / `crs:VignetteMidpoint`.
_Avoid_: vignette (bare), Post-Crop Vignette (the creative effect), edge darkening

**Chromatic Aberration**:
The corrective [[Lens Corrections]] component that removes *lateral* (transverse)
colour fringing by scaling the red and blue channels radially per the profile, so
edge colours realign. Covers fringing caused by the lens geometry, not *axial*
(defocus) purple/green fringing, which is a deferred heuristic defringe with no
profile.
_Avoid_: CA (expand on first use), defringe (the deferred axial heuristic),
purple fringing (the axial defect this does not fix)

**Grain**:
A monochromatic, zero-mean texture applied as the last develop effect in a
perceptual encoding. Its pattern is deterministic for one image and anchored to
the cropped frame, so it does not swim during preview or change on reopen.
_Avoid_: noise reduction, sensor noise, digital noise

**Spot**:
A clone-based pixel replacement applied to the decoded [[ImageBuffer]] before the
shader pipeline (but after [[Lens Corrections]], so the buffer it addresses is the
corrected negative). A destination circle (centre + radius) is filled with pixels
sampled from a source circle (same radius, user-placed offset), feathered at the
boundary. Coordinates are normalised to the (corrected) image buffer dimensions
(not the display frame) — the same pixel is addressed regardless of [[Rotation]]
or [[Crop]], because those are shader operations applied after. Applied CPU-side
to the clean decoded buffer; the shader sees only the result. Distinct from
[[Local Adjustment]], which is a tonal/colour delta evaluated in the shader — a
Spot carries no tonal parameters and is not a parametric mask.
_Avoid_: heal (implies Poisson blending, which is not implemented), clone stamp
(tool name, not the data), local adjustment (different pipeline stage)

**Colour Noise Reduction**:
The removal of *chroma* noise — the coloured blotches of high-ISO captures — by
smoothing colour while leaving luminance detail untouched. A single Amount control
(0..100) drives a separable Gaussian blur of the unit-luma chroma ratio, run as a
cached multi-pass GPU pre-pass inside [[RendererCore]] at quarter resolution,
immediately before the main shader. It samples the already-uploaded
lens-corrected/spotted texture, so it sits *last* in the pipeline — safe because the
chroma ratio is unchanged by the achromatic vignette gain and geometry-only [[Spot]]
clones. Sigma is calibrated in full-res pixels (the blur runs at sigma/4 on the
quarter-res chroma). The recompute is debounced ~200 ms after the slider settles.
Luminance noise reduction — the grainy brightness speckle — is a separate, deferred
concept. The opposite of [[Grain]], which *adds* texture. Stored as
`crs:ColorNoiseReduction`, default 0.
_Avoid_: denoise (bare — say which kind), luminance NR (the deferred sibling),
grain (the inverse operation), sharpening (a different Detail control)
