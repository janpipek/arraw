# Local adjustments: linear, radial and brush masks — plan

Status: proposed, 2026-10-08. Step (c) of `architecture-roadmap.md`.
Merges a Claude and a Codex proposal (both deleted) and Codex's review of the
former (`docs/reviews/gpt-6_2026-10-08_local-adjustment-plan.md`).

## Decisions (user, 2026-10-08)

| Question | Answer |
|---|---|
| When | After step 3 of `looks-and-history-plan.md` (session history) |
| How deltas combine | Summed in setting units, clamped once, then resolved (§2) |
| How weights reach the chain | Linear and radial evaluated analytically in the pointwise pass; brush masks as textures |
| Brush storage | Strokes, rasterised deterministically into a cache (§5) |
| First delta set | Exposure, Contrast, Highlights, Shadows, Whites, Blacks, relative Temp, relative Tint, Saturation, Vibrance, Texture, Clarity, Dehaze |
| Mixed-sign Dehaze | Both haze bases prepared when both signs are reachable; chosen per pixel (§4) |
| Unknown mask types and fields | Dropped on read with a warning (no opaque retention for now) |
| Copy/paste and presets | Masks are never copied; they belong to one photograph |
| CLI | No mask creation; masks in a sidecar are applied, `info` lists them |

## Goal

From `desired-features.md`: up to 16 masked adjustments per photograph, each
with its own tonal and colour deltas — **linear** (a graduated fade),
**radial** (an oval with feather and invert) and **brush** (a stencil painted
on the photograph that stays put under zoom, pan, crop and rotation).

The GUI, Python and the sidecar describe the same masks; preview, export,
the CLI and Python render them identically on the CPU and the GPU. No mask
kind is creatable before export renders it.

Out of scope: combining shapes inside one mask (add/subtract/intersect),
range and subject masks, auto-mask, local noise reduction / sharpening /
hue / colour overlay, spot removal, pressure sensitivity, creating masks from
the CLI, transferring masks between photographs. The model leaves room for
the first two.

## What exists

- **Frames are decided** (ADR 009): parametric shapes in the *corrected*
  frame, brush strokes in the *sensor* frame, both before orientation,
  positions normalised per axis, scalar sizes in long-edge units. Without lens
  corrections both equal the decoded source. So orientation, straighten and
  crop never rewrite a mask (unlike `origin/old`). ADR 009 itself needs no
  correction; the wrong wording it quotes was `main`'s.
- **The pointwise pass runs on source pixels** before geometry (ADR 011), so
  a pixel's position is already in the masks' frame.
  `GeometryPlan::toSource` / `toUpright` map points to and from the upright
  frame.
- **Tone resolution** (`src/core/ToneSettings.cpp`): exposure → `exp2(EV)`
  after a clamp; the four regional and endpoint shifts are `reach ·
  clamp(setting) / 100`, linear between the clamps; contrast →
  `exp2(c / 200)` and `greyPivot^(1−slope)`. The clamps carry the chain's
  monotonicity argument (ADR 013).
- **Presence** (`src/core/Presence.cpp`): the context is built from the
  pointwise input on every pointwise run (`Develop.cpp:89`,
  `GpuDevelop.cpp:318`), only when a control is on. Dehaze's base depends on
  its sign: positive uses a floor (opening + reconstruction, a narrower
  sigma), negative a broad mean.
- **Taps**: `curveInputFieldsOf` / `sameAtTap` decide when the curve-input
  histogram is stale.
- **JSON**: `SettingsJson.h` reads and writes `DevelopSettings` only.
- `origin/old` had masks (ADR 0010, 0047): useful for handle UX and naming
  ("Linear 2"), not for frames or storage.

## Design

### 1. Model (`include/LocalAdjustments.h`)

```text
DevelopState
  settings          DevelopSettings                  (unchanged)
  localAdjustments  std::vector<LocalAdjustment>     (≤ 16, ordered)

LocalAdjustment
  id        LocalAdjustmentId  unique in the photograph, never reused
  name      std::string        empty → "Linear 2" style
  enabled   bool
  opacity   float              0..1, scales every delta
  shape     Mask               variant<LinearMask, RadialMask, BrushMask>
  invert    bool
  deltas    LocalDeltas        one float per row of the local table (§2)

LinearMask  from, to: CorrectedPoint     weight 1 at `from`, 0 at `to`
RadialMask  centre: CorrectedPoint; radiusX, radiusY (long-edge units);
            angle (degrees); feather (0..1)
BrushMask   strokes: std::shared_ptr<const StrokeList>   (§5)
```

- **Edit rules in core** beside `Edits.h`: add, duplicate, remove, reorder,
  rename, set a delta, move a handle, append to a stroke. Adding or
  duplicating at 16 is refused and leaves the state unchanged. Ids come from a
  per-photograph counter persisted with the list; duplicates get new ids.
- **Validation:** every value finite; linear endpoints closer than a minimum
  distance or radii of zero are refused by the edit rules and, when read from
  a file, dropped with a warning. Handles may lie outside the image.
- **Equality** is semantic: a brush compares stroke contents only when the
  pointers differ, so an unchanged state is never a spurious edit.
- Selection, overlay visibility and the active tool are view state, not
  document state.
- `shape` can later become a list of components with an operation without
  changing anything around it.

### 2. The equations

**Local descriptor table**, next to `developSettingDescriptors`: key, range,
default 0, the global control it acts on. It drives JSON/XMP keys, Python
names, slider rows and history labels.

For every control *c* in the table and every pixel:

```
effective_c = clamp_c( global_c + Σᵢ opacityᵢ · wᵢ · δᵢ,c )
```

summed in **setting units**, clamped **once** to the global control's range,
then resolved by the same function the global setting uses. The shoulder and
everything not in the table stay global.

| Control | Local range | Resolution per pixel |
|---|---|---|
| Exposure | ±4 EV | `gain = exp2(effective EV)` |
| Highlights, Shadows, Whites, Blacks | ±100 | `reach · effective / 100`: a clamp and a multiply |
| Contrast | ±100 | `slope = exp2(effective / 200)`, `scale = greyPivot^(1−slope)`; only when a mask carries contrast (a plan flag) |
| Saturation, Vibrance | ±100 | the existing resolution, per pixel |
| Temp, Tint | ±100, relative | log-gain rule below |
| Texture, Clarity, Dehaze | ±100 | effective amount on the existing scale; §4 for the context |

**Relative Temp/Tint.** A diagonal gain in the working space, applied right
after `toWorking` (where white balance lives) and before exposure:

```
log2 g_R = +a·T − b·N/2      log2 g_G = +b·N      log2 g_B = −a·T − b·N/2
then g ← g / Y(g)   (Y = working-space luminance row: brightness unchanged)
```

with `T`, `N` the effective relative Temp and Tint (±100, no global
counterpart, so the clamp is the local range), and `a`, `b` fixed so ±100
roughly matches moving the global white balance by ±1500 K / ±40 tint around
5500 K. Because gains are exponentials of linear functions, summing in setting
units is summing log gains. Same rule for RAW and non-RAW (the working space
is shared); under Black & White it acts like a colour filter before the
mixer, which is documented rather than suppressed. The constants are fixed in
the ADR.

**Neutral cases:** no masks, all masks disabled, or all deltas zero → the
plan's local block is empty and the output is bit-identical to today. A mask
whose deltas cancel another's yields exactly the global value where both
weights are equal.

### 3. Shapes

- **Linear:** `w = smoothstep` of the projection onto `from → to`, measured in
  an isotropic metric (long-edge units), so the bands stay perpendicular to
  the line on screen.
- **Radial:** elliptic distance in the rotated, isotropic frame; `w = 1`
  inside `1 − feather`, smoothstep to 0 at the boundary; feather 0 gives a
  hard (antialiased by one pixel) edge.
- **Invert:** `w ← 1 − w`. An empty inverted brush covers the whole frame.
- The plan holds each mask resolved into source-pixel coefficients at the
  rendered level and pixel scale, so a reduced preview and a full export
  agree, and region renders evaluate the same field.

### 4. Texture, Clarity and Dehaze

- **Context requirements are separate from amounts.** The plan records which
  bases are needed from the *set of reachable effective amounts*: a base is
  active if the global amount or any enabled mask's delta is non-zero.
- **Dehaze, both signs:** if the reachable effective Dehaze includes positive
  values, the floor base is prepared; if it includes negative ones, the mean
  base is. Reachability is taken conservatively from interval arithmetic over
  global + Σ positive deltas and global + Σ negative deltas. Each pixel picks
  the floor or the mean by the sign of its effective Dehaze; exactly zero
  reads neither. Cost: a second base only when both signs are reachable.
- **Today the context is rebuilt with every pointwise run**; so is the
  local version. Caching it apart from mask geometry and amounts (keyed by
  source identity, denoise result, level and the base requirements) is an
  optional later performance step, not part of this plan's correctness.
- Tests: mixed signs across one frame, cancellation to zero, local-only
  Presence with global zero, on both backends.

### 5. Brush: a reproducible stroke contract

```text
Stroke   radius, hardness, flow (long-edge / 0..1), erase,
         points: SensorPoint[]   (pressure reserved, ignored for now)
```

- **Dabs by distance:** the polyline is resampled at a fixed spacing of
  `0.25 · radius`, in storage-frame units, so the mask never depends on the
  pointer event rate or the resolution painted at.
- **Dab:** radial profile `1` inside `hardness · radius`, smoothstep to 0 at
  `radius`. **Paint:** `m ← m + flow · d · (1 − m)`. **Erase:** `m ← m · (1
  − flow · d)`. Strokes apply in order. Pixel centres at `+0.5`.
- **Attributes are captured per stroke.**
- **Rasteriser version** stored with the encoding; a newer version re-renders
  old strokes by the old rules or refuses with a diagnostic.
- **Cache:** tiled coverage keyed by (stroke-list identity, raster size, pixel
  scale, rasteriser version). During a gesture the live stroke is a mutable
  buffer owned by the session; on commit it is frozen into a new
  `StrokeList` that shares the previous strokes (a persistent list, not a
  copy), and only dirty tiles are re-rasterised. Undo restores the previous
  list and its tiles; cancel drops the live stroke.
- **Resolution:** coverage at the rendered level (one level coarser is an
  option the prototype measures; hardness 1 is the case to judge).
- **Prototype first** (before the format is fixed): long strokes, replay
  after reload, 16 full-size brush masks, memory and time.

### 6. GPU

- Up to 16 mask descriptors and their resolved deltas in a std140 block
  (pointwise UBO or a second one), with offset `static_assert`s and the
  shader-layout reflection test.
- One shader function per shape and per equation of §2, mirrored line for
  line; parity tests including narrow feathers and overlaps.
- Brush coverage as R8/R16 textures, four packed in RGBA or a texture array;
  the ADR picks after checking format support on Vulkan, D3D11, Metal and GL
  through QRhi, and reserves bindings for the source, tone curves and
  Presence textures. Packing reduces bindings, not memory.

### 7. Plan, checkpoints and taps

- The plan's local block (masks, resolved deltas, flags, brush identities)
  joins the Pointwise group of `stagesOf`: a mask edit invalidates Pointwise
  onwards, never Denoise.
- `curveInputFieldsOf` gains the local fields that act before the
  curve-input tap (Temp/Tint, exposure, tone, Presence, geometry); local
  colour controls after the tap leave the histogram alone.

### 8. Persistence

- **State JSON:** `stateToJson(const DevelopState&)` and
  `applyStateJson(...)` beside the settings-only calls, which stay.
- **XMP:** `arraw:LocalAdjustments`, an `rdf:Seq` of structures: id, type,
  name, enabled, opacity, invert, geometry, non-zero deltas by key; the id
  counter; a list format version. Strokes as one compact text string per
  stroke with the rasteriser version (or base64 binary; the prototype
  decides), with a cap on points per stroke and strokes per mask.
- **Reading:** older documents load with no masks. Unknown mask types,
  unknown delta keys, malformed entries, duplicate ids (later ones) and
  entries past 16 are dropped with a warning naming them. Foreign XMP outside
  arraw's list is preserved as today; arraw owns and rewrites the list.
- **Copy/paste and presets never carry masks:** no copy section, `withLook`
  leaves the target's list alone.

### 9. Front ends

- **CLI:** no flags. `export` renders sidecar masks; `info` lists each mask
  (type, name, enabled, non-zero deltas; `--json` adds geometry).
- **Python:** `photo.local_adjustments` (read-only, with ids);
  `add_linear_mask(from_, to, **deltas)`, `add_radial_mask(...)`,
  `add_brush_mask(strokes, ...)`, `with_local_adjustment(id, **changes)`,
  `without_local_adjustment(id)`, returning new photos; `.pyi` regenerated;
  CPU use needs no graphics initialisation; a CLI/Python parity test over a
  sidecar with masks.
- **GUI:**
  - **Masks panel** in the develop dock: list (add Linear / Radial / Brush,
    rename, enable, invert, duplicate, delete), the selected mask's opacity
    and delta rows via `SettingPresentation`.
  - **Mask tool** (a controller like `CropModeController`, outside
    `MainWindow`): linear handles (both ends, centre), radial handles
    (centre, two radii, rotation, feather), drawn through view →
    composition → upright → source.
  - **Overlay:** red tint of the selected mask's coverage, O to toggle, from
    the same coverage functions as development (CPU at view resolution, or a
    tap).
  - **Brush tool:** size, hardness, flow, erase (Alt), `[` / `]`, a cursor
    showing radius and hardness.
  - **Gestures:** one handle drag, slider drag or stroke is one
    begin/update/commit edit (ADR 022). Entering crop mode, switching tool or
    photograph, or losing pointer capture cancels a gesture in progress. Mask
    and crop modes are exclusive; pan stays on Space.
  - **History origins:** Mask added / moved / painted / changed / removed,
    labelled from the local table.

## Steps

Each step: Sonnet workflow → Opus review (`docs/reviews/`) → the user
decides the commit. Intermediate commits inside a step are fine as long as no
user-reachable path creates a mask that export ignores.

0. **ADR 044, masks:** §1–§8 with the Temp/Tint constants, the Dehaze
   reachability rule, the stroke contract and the brush texture packing left
   open for step 5. No code.
1. **Restructuring, no output change (done, 2026-10-09):** one stage table and driver for CPU
   and GPU, an engine-owned `CheckpointLadder`, `PointwisePlan` / `TonePlan`
   with the chain taking sub-blocks and per-pixel resolution functions
   shared by both. All parity and golden tests unchanged.
2. **Linear and radial, end to end without GUI:** model and edit rules,
   the local table, the equations, CPU and GPU, Presence requirements, taps,
   state JSON, XMP, `info`, Python. Anchoring tests under crop, straighten,
   all orientations and flips; preview vs export; region renders; odd and
   non-square sizes; overlaps, cancellation, limits; round trips including
   disabled and inverted masks.
3. **Linear and radial GUI:** panel, mask tool, overlay, gestures, history.
   Measure slider and drag response with 16 masks.
4. **Brush prototype (done, 2026-10-10):** the stroke contract and cache,
   measured; encoding and texture packing fixed; ADR 044 amended. See
   `docs/ideas/brush-prototype-report.md`.
5. **Brush, end to end without GUI:** rasteriser, cache, GPU textures,
   parity, persistence, Python. Tests: same path at different event rates,
   different resolutions, after reload.
6. **Brush GUI:** the tool, dirty-tile painting, cursor, history. Measure a
   long painting session.

## Open (for the ADR)

- Temp/Tint constants `a`, `b`, and whether the luminance normalisation
  should use the working space's or the as-shot luminance row.
- ~~Brush coverage resolution and texture packing~~: settled by the brush
  prototype (`docs/ideas/brush-prototype-report.md`). Full rendered-source
  size; RGBA8 four per texture with an ordered dither (RGBA16F if the dither is
  unwanted).
- ~~Stroke encoding~~: settled. Base64-delta per stroke, with caps of 10 000
  points per stroke, 2 000 strokes and 100 000 points per mask, plus swept-area
  and dab budgets.
- For step 5: rasterise the displayed size first on open; full size lazily or
  in bands at export.
- Whether to cache the Presence context apart from the pointwise run.
- Lens corrections later: parametric masks move with a new profile, brush
  strokes do not (ADR 009 already accepts this).
