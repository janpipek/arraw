# The brush, end to end without a GUI — plan

Status: proposed, 2026-10-10; revised after review
(`docs/reviews/claude-opus-5-5_2026-10-10_brush-end-to-end-plan.md`); the user's
decisions of 2026-10-10 folded in (end of the document). Step 5 of `local-adjustment-plan.md`: "Brush, end
to end without GUI: rasteriser, cache, GPU textures, parity, persistence,
Python. Tests: same path at different event rates, different resolutions,
after reload."

Contract: ADR 044 (§1 model and edit rules, §6 stroke contract, §7 GPU and
coverage packing, §8 plan and taps, §9 persistence, §10 front ends), as
amended 2026-10-10. Measurements and accepted options:
`brush-prototype-report.md` (1a, 2b, 3a, 4b, 5a). Prototype code:
`src/core/{BrushStrokes,BrushRaster,BrushCoverageCache,StrokeCodec}`. Template
for every touch point: step 2, commit d9c0bfb (linear and radial, end to end).

Out of scope (step 6): the brush tool, the live stroke and its tail method,
the cursor, painting in the overlay, a long painting session's measurements.
Also out: a background prefetch of full-size coverage, lens corrections.

## Decisions at a glance

Fixed here; the user's answers are listed under "Decisions (user, 2026-10-10)" at the end.

| Topic | Decision |
|---|---|
| Public types | `SensorPoint`, `Stroke`, `StrokeBudget`, `StrokeList`, the caps and `budgetOf`/`validate`/`normalised` move to `include/BrushStrokes.h`; `BrushMask` joins `Mask` |
| New edit rule | `withStrokeAppended(state, id, Stroke)` and `canAppendStroke(state, id, stroke)`; adding a brush goes through `withLocalAdjustmentAdded(state, Mask)` |
| Who owns float coverage | The engine: one process-wide `BrushCoverageCache` (512 MiB, 128-px tiles), engine-private |
| Who owns packed coverage | The `CheckpointLadder` that renders (one residency per ladder: host RGBA8 planes, and textures on a GPU ladder) |
| Window renders (through a ladder) | Coverage from the cache, packed into the ladder's planes; only tiles whose serial changed are repacked and uploaded |
| Direct renders (export, thumbnails, Python, CLI, samples) | Up to `retainedCoveragePixels` (2048², the window's sizes, the curve histogram, thumbnails): through the cache, retained. Above it: cache hit-only lookup, else a banded raster straight into fresh RGBA8 planes; nothing retained |
| Size | The rendered source's size (pyramid level, half-size decode or full); never a prefetch of full size |
| GPU uploads | Pending rectangles per texture, kept until an upload completes (survive a cancel, and a CPU render through the same ladder) |
| What both backends read | The same dithered RGBA8 texels: the CPU reads the host planes, the GPU the textures made from them |
| Dither | 4×4 Bayer, phase = the mask's index in `state.localAdjustments` |
| Dirty tiles | Compared by a per-tile serial number (not the pointer: no ABA, no retention) |
| Progress | A new `ProgressStep::Coverage` between `Context` and `Pointwise` (decision 2a) |
| Persistence | JSON readable strokes, XMP one base64 string per stroke in an `rdf:Seq`; reading rules in the format-neutral `readLocalAdjustments` |
| GUI | Listed, named ("Brush 1"), worded in history, deletable, its deltas editable, tinted by O; no creation, no handles, no pin |
| Tint (O) of a brush | `maskCoverage` supports a brush: float coverage from the shared cache at a pyramid size no coarser than the grid, on the GUI thread (decision 6b, §7.1) |
| Level-0 window render that would draw coverage from nothing | The window first renders and shows level 1, then level 0 (decision 9b, §2.10) |
| Sub-steps | 5.1 model + CPU direct; 5.2 retained coverage + the coarser first render (9b); 5.3 GPU; 5.4 persistence + GUI (tint, 6b) + CLI; 5.5 Python + docs |

## 1. Public API

### 1.1 `include/BrushStrokes.h` (moved from `src/core/BrushStrokes.h`)

Moved as it is, with Doxygen `@brief` briefs like the other public headers:
`brushRasteriserVersion`, `maximumStrokePoints`, `maximumStrokesPerMask`,
`maximumPointsPerMask`, `maximumSweptAreaPerMask`, `maximumDabsPerMask`,
`minimumBrushRadius`, `maximumBrushRadius`, `SensorPoint`, `Stroke`,
`StrokeBudget`, `budgetOf`, `validate(const Stroke&)`, `normalised(Stroke)`,
`StrokeList`.

Changes on the way:
- `StrokeList::contentHash()` is documented as stable across processes and
  builds (FNV-1a over the bytes as defined in `BrushStrokes.cpp`): the
  thumbnail key stores it (§3.3). Changing its definition is allowed but
  invalidates thumbnail keys only.
- `StrokeList(std::vector<Stroke>, std::uint32_t rasteriser)` keeps accepting
  any rasteriser number (a reader needs to build and then refuse); `validate`
  of a state refuses one other than `brushRasteriserVersion` (§1.3).
- `emptyStrokeList()` (new): a shared, immutable empty list of the current
  rasteriser, the default of `BrushMask::strokes`.

`src/core/BrushRaster.h`, `BrushCoverageCache.h` and `StrokeCodec.h` stay
private. `BrushStrokes.cpp` keeps including `<LocalAdjustments.h>` for the
position range; `LocalAdjustments.h` includes `<BrushStrokes.h>`. No header
cycle.

### 1.2 `include/LocalAdjustments.h`

```cpp
/// @brief A stencil painted on the photograph: strokes in the sensor frame (ADR 044, §6).
struct BrushMask {
    /// @brief The strokes, shared and immutable; never null in a valid state.
    std::shared_ptr<const StrokeList> strokes = emptyStrokeList();

    /// Equal when the pointers are, or failing that the lists' contents (ADR 044, §1).
    friend bool operator==(const BrushMask& a, const BrushMask& b);
};

using Mask = std::variant<LinearMask, RadialMask, BrushMask>;
```

- `maskTypeName` gives `brush`.
- `normalised(const Mask&)` for a brush: a null `strokes` becomes
  `emptyStrokeList()`; otherwise the list is returned as it is (a `StrokeList`
  holds its caps by construction). An unknown rasteriser throws
  `std::invalid_argument`.
- `LocalAdjustment` equality is unchanged (defaulted); it picks up
  `BrushMask`'s.
- A copied `DevelopState` shares the lists: copying a state with 16 full
  brushes copies 16 pointers.

### 1.3 Validation (`validate(const LocalAdjustment&)`, `validate(const DevelopState&)`)

A brush mask is refused when `strokes` is null or its rasteriser is not
`brushRasteriserVersion`. Strokes need no further check: the only ways to make
a `StrokeList` validate each stroke and every cap and budget.

### 1.4 Edit rules (`include/LocalAdjustmentEdits.h`)

```cpp
/// @brief Tells whether a stroke can be appended to a brush mask: it normalises, and the
/// mask stays within every cap and budget (what a front end shows as "mask full").
[[nodiscard]] bool canAppendStroke(const DevelopState& state, LocalAdjustmentId id,
                                   const Stroke& stroke) noexcept;

/// @brief Appends a stroke to a brush mask, normalised (`normalised(Stroke)`).
/// @throws std::invalid_argument if @p id is not in the list or not a brush mask, the stroke
/// cannot be normalised, or the mask would pass a cap or budget; @p state is then unchanged.
[[nodiscard]] DevelopState withStrokeAppended(DevelopState state, LocalAdjustmentId id,
                                              Stroke stroke);
```

- Adding a brush: `withLocalAdjustmentAdded(state, Mask{BrushMask{list}})`, as
  for the other kinds. An empty brush is a valid mask (nothing painted yet).
- `withLocalShape(state, id, BrushMask)` replaces the strokes whole (same-kind
  rule as today); Python's `with_local_adjustment(shape=...)` uses it.
- Duplicate shares the list (pointer copy).
- `maskOrdinal`, `defaultMaskName` ("Brush 2"), `displayedMaskName` count and
  name by `shape.index()`; every `index() == 0 ? "Linear" : "Radial"` becomes a
  three-way switch.
- No removal of single strokes: undo restores the previous list.

### 1.5 `describeChange` (`include/EditSession.h`)

`MaskChange` gains `bool strokes` ("the brush's strokes differ"). `shape`
keeps meaning geometry or kind; for two brush masks it is false and `strokes`
says whether the lists differ (by `BrushMask` equality).

### 1.6 Every place that inspects the variant

These must handle three alternatives in the sub-step that introduces
`BrushMask` (step 5.1); `holds_alternative<LinearMask> ? … : <radial>` would
silently treat a brush as radial:

| File | What it does today | For a brush |
|---|---|---|
| `src/core/LocalAdjustments.cpp` | name, `normalised`, `validate` (visit) | §1.2, §1.3 |
| `src/core/LocalAdjustmentEdits.cpp` | ordinal, default name | §1.4 |
| `src/core/LocalPlan.cpp` | `resolvedMask` | §3.1 |
| `src/core/DevelopedFrame.cpp` | `maskCoverage` | step 5.1: refuses a brush (`std::invalid_argument`); step 5.4: §7.1 |
| `src/core/SettingsJson.cpp`, `LocalAdjustmentCodec.cpp` | write geometry | step 5.1: throw `std::logic_error("brush masks are not persisted yet")`; step 5.4: §5 |
| `src/core/EditSession.cpp` | `describeChange` | §1.5 |
| `src/app/MasksModel.cpp` | `KindRole` | 3 |
| `src/app/MaskPresentation.cpp` | `maskDisplayName` | `tr("Brush %1")` |
| `src/app/MaskEditing.cpp` | handles, pins, drags | none (§7) |
| `src/app/ui/MaskOverlay.cpp` | paints handles, pins and the tint | no handles or pins; step 5.1: no tint (the refusal is caught, as today); step 5.4: the tint (§7.1) |
| `src/python/BindSettings.cpp`, `BindPhoto.cpp` | bind shapes | step 5.1: the `Stroke` and `BrushMask` types (§6.1), so that a state holding a brush converts; step 5.5: the methods (§6.2) |
| `src/cli/InfoCommand.cpp` | prints masks | §6.3 |

The app and Python sites change in step 5.1 too (they must compile and must
not misbehave), although no reader or GUI action makes a brush before step
5.4. From step 5.1 Python could build one by hand (`DevelopState` with a
`LocalAdjustment` whose shape is a `BrushMask`): it renders on the CPU, and
the GPU and the sidecar writer refuse it loudly until steps 5.3 and 5.4.

## 2. Coverage in the render path

### 2.1 Size, levels, regions, pixel scale

- Coverage is rasterised at the **size of the source the render develops**:
  the pyramid level the preview picked, a half-size decode, or the full
  source. One coverage pixel per source pixel (ADR 044 §6). Strokes are
  rasterised directly at that size, never resampled from another.
- Pixel scale plays no part: radii are long-edge units, so the size alone
  fixes the pixels.
- Region renders: the pointwise pass covers the whole source and the region
  is cut after geometry (ADR 025), so coverage is the whole source's. When the
  early stages are later restricted to a region's footprint, coverage must be
  cut at the same footprint (ADR 044 §4); nothing to do now.
- "Rasterise the displayed size first on open, full size lazily": satisfied
  by construction. The window renders only the level it shows (and the
  background layer's coarser one); full size is made only by a render at
  level 0 or an export. Level 0 is a 1:1 view, but also a fit view on a
  display about as wide as the photograph (a 24 MP photograph on a 5K
  display, 12 MP on 4K), so opening such a photograph with heavy brushes pays
  the full-size raster at once. The window therefore shows level 1 first when
  a level-0 render would draw coverage from nothing (decision 9b, §2.10). No
  prefetch. A background prefetch is a later option if 1:1 latency on heavy
  masks proves annoying.

### 2.2 Who owns what

| Holder | What | Lifetime | Why there |
|---|---|---|---|
| Engine, process-wide: `detail::brushCoverageCache()` (`src/core/BrushCoverage.h`) | Float coverage tiles (`BrushCoverageCache`, 512 MiB LRU, 128-px tiles) | Process | Coverage depends on strokes and size only, not on the photograph's pixels or the caller; the cache is already thread-safe. Not `Photo` (a copied value), not `EditSession` (no pixels, absent in the CLI). |
| `CheckpointLadder` (private member, engine-private type) | `detail::CoverageResidency`: the packed RGBA8 host planes at the ladder's source size, one record per slot, and on a GPU ladder the textures | As the ladder's rungs: dropped when the ladder rebinds to another source or is cleared | The ladder is already the per-caller, per-source, per-device render state; GPU textures belong to the same thread as its resident rungs. |
| A direct render (no ladder) | A transient `detail::PackedCoverage` | The call | No packed planes outlive the call. Float coverage goes into the cache only up to `retainedCoveragePixels` (§2.5), so a full-size export never fills it. |

`CheckpointLadder` gains `std::shared_ptr<detail::CoverageResidency> coverage_`
(the type is forward-declared, as `CheckpointState` is). `LadderAccess` gains
`coverage(CheckpointLadder&)`, creating it on first use. `LadderAccess::bind`
to another source and `CheckpointLadder::clear` drop it. A copied ladder
starts with no residency (the copy constructor and assignment reset
`coverage_`), so two ladders never write the same planes. A render of a plan
with no brush drops the residency too (an implementer's choice, kept: it frees
up to 366 MiB at 24 MP; toggling the only brush off and on then repacks every
brush from the cache, and redraws those the cache evicted). The GPU part of a
residency is a `detail::DeviceCoverage` interface declared in core and
implemented in `src/gpu` (the `DeviceImageState` pattern), so core never names
Qt.

The process-wide cache is a function-local static that is never destroyed
(allocated once and leaked), so that a render thread still running during
static destruction, such as Python's at interpreter exit, cannot touch a dead
cache.

### 2.3 The cache, as it becomes

From the prototype, with:
- `CoverageTile` gains `std::uint64_t serial`, unique in the process (an atomic
  counter, assigned when a tile is made or cloned for painting). Residencies
  compare serials to find changed tiles. Pointers are not compared: a freed
  tile's address can be reused (ABA), and keeping the old grids alive to
  prevent that would defeat the budget.
- `find(strokes, raster)`: a hit-only lookup (`Hit` or `ContentHit`, no insert,
  no extension, no drawing); empty otherwise. Used by direct renders.
- `peek(strokes, raster)`: the kind `coverage` would give (`Hit`, `ContentHit`,
  `Extended` or `Miss`) without drawing, inserting or touching the LRU order.
  Used by §2.10 (step 5.2).
- `Result::dirty` stays a hint for tests.
- `drawn` (a `Miss` or `Extended`) paints its dirty tiles from the same
  row-bucket index as the banded path (§2.5), one bucket per tile row (128
  rows), not with `paintRegion` over every placed stroke per tile. Otherwise
  a frame-crossing stroke at the dab budget would loop up to 2·10⁶ dabs for
  each of about 1 500 tiles at 24 MP. Bits are unchanged by construction.
  Step 5.2 measures a cache miss of B2's 16 masks at 24 MP beside the banded
  path.
- Tile size and budget stay 128 px and 512 MiB (report 5a). Constants, not
  settings.

### 2.4 Quantising and packing (shared by both paths and both backends)

`src/core/BrushCoverage.h`:

```text
bayer4 = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5}       (row-major)
threshold(x, y, phase) = (bayer4[((y + (phase >> 2)) & 3) * 4 + ((x + phase) & 3)] + 0.5F) / 16.0F
code(m, x, y, phase)   = min(255, floor(clamp(m, 0, 1) * 255.0F + threshold(x, y, phase)))
weight(code)           = float(code) / 255.0F
```

- `(x, y)` are the raster pixel's column and row; `phase` 0 to 15 is the mask's
  index in `state.localAdjustments` (decision 3a). The 16 phases are 16 distinct
  shifts of the matrix, so no two masks of one state dither in step.
- `m = 0` gives code 0 and `m = 1` gives 255 exactly, so outside every stroke a
  brush contributes exactly zero and the pixel takes the global path bit for
  bit, as with linear and radial masks.
- Float arithmetic, no contraction (the `arraw` target is already built with
  `-ffp-contract=off`).
- **Slots.** The plan numbers the brush masks it keeps 0 to 15 in list order
  (`slot`). Slot `s` lives in plane (texture) `s / 4`, channel `s % 4`; a plan
  with `n` brushes has `ceil(n / 4)` planes. A plane is `width * height * 4`
  bytes, RGBA, row-major: exactly a texture's upload layout.
- Channels of a plane that no slot uses are never read (the shader and the CPU
  read only `channel` of a mask's slot); their bytes are unspecified.

### 2.5 The direct path (no ladder)

`detail::packCoverage(const LocalPlan&, ImageSize) -> PackedCoverage`, for
`develop`, `developUntil`, `resumeFrom`, `sample`, `developOnGpu` (all three
overloads, the checkpoint resume included) and `sampleOnGpu` (both):

0. If the raster has at most `retainedCoveragePixels` = 2048 × 2048 pixels,
   each brush goes through `brushCoverageCache().coverage(strokes, size)`
   (retained) and its tiles are quantised as on a hit, below. These are the
   sizes the window renders, its curve histogram (`sample` at 1024 or 512,
   recounted on every pre-tap edit), thumbnails and Python previews. Without
   retention each recount would rasterise every pre-tap brush again (about
   1 s for 16 heavy masks at 1 to 2 MP). Larger rasters, such as an export,
   never fill the cache (ADR 044 §6) and take steps 1 to 3.
1. For each brush in the plan, `brushCoverageCache().find(strokes, size)`. On a
   hit, quantise its tiles into the slot's channel (a null tile writes zeros).
2. Otherwise rasterise **banded**, outside the cache:
   - place the strokes once (`placedStrokes`);
   - build a bucket index: rows in buckets of 64; for each bucket, the
     `(stroke, dab)` pairs whose row span (`dabSpan`) meets it, in painting
     order (strokes in list order, dabs in path order). A dab spanning two
     buckets is listed in both; each bucket paints only its own rows, so the
     bits are the reference's;
   - `forEachRowBand` over buckets (rows = bucket count, width = bucket rows ×
     raster width), so cancellation and progress fall between buckets; each
     bucket paints its dabs onto a zeroed float scratch of its rows (the
     `paintRegion` loop over the listed dabs), then quantises into the plane.
   Peak memory: the planes (1 byte per pixel per brush, rounded up to four
   brushes) plus one bucket's floats per thread. 16 brushes at 24 MP: 366 MiB
   of planes, about 1 MiB of scratch per thread, where float planes would be
   1.4 GiB.
3. Nothing is inserted into the cache.

Equal to quantising `rasteriseBrush` bit for bit, for any thread count, bucket
height and band split, and whichever of steps 0 to 2 served it (tested).

The per-call `paintRegion` over a whole stroke's dabs is not used for buckets:
observed bands run in chunks of about 64 K pixels (`RowBands.h`), and looping
every dab of a frame-crossing stroke once per chunk would add seconds per heavy
mask at 24 MP.

### 2.6 The retained path (through a ladder)

`detail::CoverageResidency::update(const LocalPlan&, ImageSize) -> const
PackedCoverage&`, for `resumeOrDevelop` and `resumeOrDevelopOnGpu`:

```text
residency: size; planes[0..3] (host RGBA8, made on first use, all of `size`);
           record[0..15] = { BrushCoverageRef identity; std::vector<std::uint64_t> serials }
                           (serial 0 = an absent, all-zero tile)

update(local, size):
  if size differs from the residency's: drop planes and records
  for each brush slot s in local:
    if record[s].identity == the slot's BrushCoverageRef: nothing to do      (a delta drag)
    tiles = brushCoverageCache().coverage(strokes, size)                      (Hit / Extended / Miss)
    changed = every tile if the record is empty or its phase differs,
              else the tiles whose serial differs
    quantise the changed tiles into plane s/4, channel s%4; record the serials and identity
    add the changed tiles' rectangles to texture s/4's pending list   (either backend; see below)
  forget records of slots the plan no longer has
```

- A delta, opacity or invert edit of any mask, and any edit of a linear or
  radial one, leaves every brush's identity equal: no cache call, no packing,
  no upload. Disabling or removing an earlier brush moves the later brushes'
  slots: they are repacked from the cache (a hit), not redrawn.
- An appended stroke (step 6's painting; today only a state change through
  the window's history can do it) gets an `Extended` result whose untouched
  tiles keep their serials: only touched tiles are repacked and uploaded.
- Undo restores the previous list; the cache still holds its entry (`Hit`), and
  only the tiles that differ by serial are repacked.
- An evicted entry is redrawn (`Miss`, new serials, the whole channel); the
  result is still bit-identical.
- The residency's planes are what the CPU pass reads; the GPU residency
  uploads only the union of changed tile rectangles per texture (all four
  channels of each texel, from the host plane).
- **Pending uploads.** `update` does not upload; it adds each repacked
  rectangle to a per-texture pending list kept in the residency, whichever
  backend called it. Only a GPU upload that completed clears a texture's list.
  A render cancelled between `update` and the upload (`GpuContext::render`
  throws `Cancelled` between passes, the Presence passes included) and a CPU
  render through the same ladder therefore both leave the rectangles pending,
  and the next GPU render uploads them. Without this, the records would say
  "current" while the textures held older codes. The textures remember the
  device that made them. On another device, or when they do not exist yet,
  they are made whole from the planes and the pending lists are cleared.

Correctness never relies on the residency or the cache: every path gives the
direct path's bits (tested).

### 2.7 How the CPU pass reads it

- `CpuStages` gains `detail::CoverageResidency* coverage` (null for direct
  renders). `runPointwise`, when `plan.pointwise.local` has a brush, opens the
  Coverage progress span, gets a `PackedCoverage` (residency `update` or
  `packCoverage`), then runs the chain.
- `developSamples` passes the pixel's 16 codes (four RGBA texels: 16 bytes, read
  once per pixel from the planes that exist) to the chain; `amountsAt(plan,
  column, row, const PixelCoverage&)` passes them to `localSumsAt`, and
  `maskWeight` for a brush is `weight(code of its slot)`, inverted when
  `invert` is set. Linear and radial are unchanged.
- A plan without brushes takes exactly today's path (no planes, no extra
  reads).

### 2.8 Cancellation and progress

- `ProgressStep::Coverage` (decision 2a), between `Context` and `Pointwise`;
  the Pointwise row of the stage table still runs `Context` to `Pointwise`.
  `RenderActivity` words it "Painting brush masks…".
- Its weight (`renderStepWeights`): `coverageCost * Σ sweptArea_i * L²` over
  the plan's brushes (the time model of report B7, `t ≈ 1.0 s · A · (L/6000)²`)
  plus `packCost * pixels * brushes`, constants measured in step 5.2 as the other
  costs were. Only brushes whose coverage is not ready count. A brush
  counts nothing when the ladder's residency already holds its
  `BrushCoverageRef`. It counts only `packCost` when the cache holds it
  (`find`, which is cheap). Both are asked before the progress root is made.
  Otherwise, at 24 MP with heavy brushes, the coverage weight (seconds of
  model) would dwarf the pointwise pass, and the pie would sit near zero then
  jump on every delta drag that does no coverage work at all.
- Units of the span: one per brush that has work, weighted by its modelled
  cost. A unit is itself divided into two parts, the drawing and then the
  packing, with their own weights, so that the pie moves through both loops
  (a brush only packed has a drawing of weight 0). Of an `Extended` brush
  only the strokes after the cached prefix are weighted
  (`BrushCoverageCache::peek` also returns how many strokes the cache holds).
  The drawing counts as done when the cache answers without drawing, so the
  declared units are run whatever another thread did to the cache meanwhile.
- Cancellation: the banded raster, the cache's `drawn` and the residency's
  packing all loop through `forEachRowBand`, so they stop within a chunk. A
  cancelled draw inserts nothing; a cancelled residency update leaves the
  record of the slot being updated cleared (so the next update redoes it) and
  the earlier slots valid. Rungs are untouched, as today.

### 2.9 Costs to expect (from the report)

| Case | Cost |
|---|---|
| Delta drag on a brush at preview size | No coverage work at all |
| Opening a photograph with 16 heavy brushes, preview at 1500×1000 | About 16 × 0.06 s on 8 threads, then retained |
| The same on a display that fits the photograph at level 0 (24 MP on 5K) | Level 1 shown after about 3 s, level 0 about 13 s later, both cancellable (§2.10) |
| Curve histogram recount after a pre-tap edit | No coverage work after the first (retained, §2.5 step 0) |
| 1:1 view of 16 realistic brushes at 24 MP | About 13 s once (cancellable); over the cache budget, so a later stroke edit may redraw evicted masks |
| Export, 16 realistic brushes at 24 MP | About 13 s banded; 366 MiB of planes |
| One brush at the budget's worst, 24 MP | About 4 s |
| Tint (O) of a selected brush, coverage cached (the usual case) | Sampling the grid: a few ms (§7.1) |
| Tint of a selected brush, cache miss | One brush at the grid's level on the GUI thread: tens of ms at a fit view, up to about a second at 1:1; zoomed past 1:1 up to the full-size cost above, but then usually already cached by the window's render (§7.1) |

### 2.10 The coarser first render (decision 9b)

When a level-0 window render would draw a brush's coverage from nothing, the
window first renders the same request from level 1, shows it, then renders
level 0.

- **The question.** `include/Develop.h` gains

  ```cpp
  /// @brief Tells whether a render through a ladder would rasterise a brush mask's coverage
  /// from nothing, rather than reuse or extend coverage already made.
  ///
  /// True when some brush of the plan is neither held by the ladder's residency nor in the
  /// shared cache, whole or as a list it extends. Draws, inserts and packs nothing.
  [[nodiscard]] bool drawsBrushCoverage(const CheckpointLadder& ladder, const ImageBuffer& source,
                                        const DevelopState& state, const RenderRequest& request);
  ```

  It shares its per-brush test with the progress weights of §2.8: the
  residency's record equals the brush's `BrushCoverageRef`, else
  `BrushCoverageCache::peek` at the source's size. Only `Miss` counts: a hit
  or an `Extended` (an appended stroke, step 6's painting) does not, so a
  delta drag, an undo or a stroke never gets a coarser first render.
- **Where.** `PreviewRenderer`'s render of a request, `Layer::Shown` only (the
  background and the curve histogram never). It applies when the level chosen
  is 0, the source has a level 1 (`SourcePyramid::highestLevel` at least 1),
  and `drawsBrushCoverage` is true for the ladder this render would go
  through: the GPU ladder when the GPU renders, else the CPU one. It takes a
  `minimumSeconds` (default 0, any brush drawn from nothing); the window passes
  0.3 s (`defaultStandInSeconds`, settable on `PreviewRenderer` for tests), the
  modelled drawing time of §2.8 of the brushes drawn from nothing. Under it the
  stand-in, a full render of a quarter of the pixels that reuses no saved
  stage, would cost about what it hides (a first short stroke, a small brush,
  a zoom to 1:1 over a level already shown). It is false when the ladder holds
  a usable rung at the pointwise pass or beyond, which the render resumes from.
- **The coarser render.** The same request, developed from level 1 as a
  direct render (`develop`, or `developOnGpu` with the uploaded level 1; a GPU
  failure falls back to the CPU as for any render): no ladder is rebound, so
  the shown ladder keeps its level-0 rungs and residency and the background
  ladder its own, and no level-1 rungs are kept. Its coverage follows §2.5
  (retained in the cache up to `retainedCoveragePixels`, banded above). This
  is a whole render of level 1 with coverage at level 1's own size, so it does
  not depart from ADR 044 §6 (one coverage pixel per source pixel).
- **Delivery.** `PreviewResult` gains `bool provisional` ("a coarser stand-in;
  the render of the same request at its own level follows"). The coarser
  result has `level` 1, its region snapped to level-1 pixels as for any level,
  and `provisional` set. Then level 0 renders through the ladder as today and
  is delivered as usual.
- **The window** (`MainWindow`) shows a provisional image as any other, a 1:1
  view enlarging it. It does not take a thumbnail from it, does not keep it as
  the background, and does not end the render indicator for the request. A
  newer request cancels either render, as today; when the level-0 render is
  cancelled, the provisional image stays until the next result.
- **Progress.** Both renders report through the request's channel, mapped into
  one rising fraction: the level-1 render fills the first fifth (a level has a
  quarter of the pixels), level 0 the rest. Each shows its Coverage step.
- **Cost.** Level 1 has a quarter of the pixels: for 16 heavy brushes at
  24 MP, an image after about 3 s instead of 13 s, and level 0 about 13 s
  after that (about 16 s in all). Once level 0's coverage is in the residency
  or the cache, no coarser render is made. Above the cache budget (16
  realistic brushes at 24 MP), returning to 1:1 after an eviction makes one
  again, which is the rule working as meant.

## 3. The plan's local block and taps

### 3.1 `LocalPlan` (`src/core/LocalPlan.h`)

```cpp
enum class LocalMaskKind : std::uint32_t { Linear = 0, Radial = 1, Brush = 2 };

/// @brief Where a brush mask's coverage comes from, and where it lives.
struct BrushCoverageRef {
    std::shared_ptr<const StrokeList> strokes; ///< Null for linear and radial masks.
    ImageSize raster;                          ///< The rendered source's size.
    std::uint32_t slot = 0;  ///< 0 to 15, in plan order: plane slot / 4, channel slot % 4.
    std::uint32_t phase = 0; ///< Dither phase: the mask's index in the state's list.
    /// Equal sizes, slots and phases, and the same pointer or equal lists.
    friend bool operator==(const BrushCoverageRef&, const BrushCoverageRef&);
};
```

- `LocalMaskPlan` gains `BrushCoverageRef brush`. For a brush, the shape
  coefficients stay zero. `LocalPlan` gains `brushCount()`.
- `localPlanFor` keeps a brush exactly as it keeps the other kinds: disabled
  or all-zero `k` is left out (no raster), and an empty brush with a non-zero
  `k` is kept (it still counts for Presence reach, ADR 044 §5).
- List equality compares pointers first, then the content hash, then per
  stroke pointer-or-contents (the existing `StrokeList ==`). After a reload
  the pointers differ and contents are compared: about 2 ms for 16 full masks,
  per rung check.
- The block stays in the Pointwise group of `stagesOf`: a brush edit
  invalidates Pointwise onwards, never Denoise.

### 3.2 Taps

`PreTapMask` gains the brush's strokes, raster size and phase. It does not
gain the slot, which is only where the codes are stored: disabling an earlier
brush that carries only Saturation moves later brushes' slots without
changing their codes, and must not recount the curve histogram. A Saturation-
or Vibrance-only brush leaves `sameAtTap` true for a stroke edit. A pre-tap
brush whose strokes or phase change makes it false.

### 3.3 Thumbnails and digests

- The developed-thumbnail key (`ThumbnailCache::developedKey`) writes brush
  masks with `StrokeDetail::Digest` (§5.1): rasteriser, stroke count and the
  content hash, not the points. Keys of photographs without brushes are
  unchanged.
- `tests/support/RenderDigest.h` gains brush states after `digestMaskStates`'s
  current lines, so older digests stay a prefix (§8.4).

## 4. GPU

### 4.1 Textures and their uploads

- `GpuContext::uploadCoverage(ImageSize, std::span<const std::uint8_t>)` makes
  an RGBA8 `DeviceImage` (`format()` `RgbaU8`, `workingEncoding` as a
  placeholder, pixel scale 1) from a plane. No format check: every QRhi backend
  has RGBA8. The texture is `QRhiTexture::RGBA8` **without** the `sRGB` flag
  (a driver would otherwise linearise the codes). `RhiDeviceImage` today
  hard-codes `PixelFormat::RgbaF32` and a channel count from RGBA32F or R32F.
  Its constructor takes the format, and `readBack` reads 4 bytes per texel for
  a coverage image. The edge and transfer-size checks of `upload` apply.
- `GpuContext::updateCoverage(const DeviceImage&, std::span<const PixelRect>,
  std::span<const std::uint8_t> plane)` uploads rectangles of a plane into an
  existing coverage image, each copied to a contiguous buffer, all in one
  resource update batch. This is the one documented exception to
  `DeviceImage`'s immutability: a coverage image is never a checkpoint's
  pixels, is updated only by the residency that made it, on the owner thread,
  between renders (which wait for completion). `DeviceImage::format()`'s note
  changes to say `RgbaU8` occurs for coverage.
- `readBack` of a coverage image gives an `RgbaU8` buffer (for tests).
- Direct GPU renders upload each plane whole; the GPU residency uploads
  changed rectangles (§2.6). A plan with no brush uploads nothing.

### 4.2 The pass

- `GpuPass::Pointwise` takes eleven inputs: today's seven, then four coverage
  textures at bindings 8 to 11 (`inputCountOf`; input `i ≥ 1` binds at
  `i + 1`). A texture the plan does not use is the image, as for every absent
  grid.
- `GpuLocalMask.header`: `x` kind (2 for a brush), `y` invert, `z` plane
  (`slot / 4`), `w` channel (`slot % 4`), as ADR 044 §7 reserved. The block's
  size and offsets do not change; the phase is not uploaded (the CPU
  quantises).
- `develop.frag`:

  ```glsl
  layout(binding = 8) uniform sampler2D coverage0;   // ... 11: coverage3
  const uint maskKindBrush = 2u;
  // brushWeight: weight(code), LocalPlan.h / BrushCoverage.h.
  float brushWeight(LocalMask mask, ivec2 at) {
      vec4 texel;
      switch (mask.header.z) {           // a switch, not a sampler array: GLSL ES 3.0 safe
      case 0u: texel = texelFetch(coverage0, at, 0); break;
      ...
      }
      return texel[mask.header.w];
  }
  ```

  `maskWeight` takes the pixel's `ivec2` too; `amountsAt` passes it.
- `pointwiseOnGpu` and `GpuStages` take the coverage source (GPU residency or
  transient planes), as the CPU does.
- `GpuPlan.h`'s `AfterMatrix` probe and Presence inputs are unchanged.

### 4.3 Parity

Both backends read the same codes. Every API defines a UNORM8 fetch as
`c / 255`, which is the CPU's `float(code) / 255.0F`; any rounding residual of
a driver is far inside the pointwise tolerance, 3e-5 relative (ADR 041). The texels themselves
are tested for exact equality (readback against the host plane).

Cases (each CPU against GPU, 3e-5 relative): each control alone on a brush;
all controls; a brush with erase strokes; inverted brush, and an inverted
empty brush; 16 brushes (four textures, all channels); brushes mixed with
linear and radial masks; brush coverage summing with a cancelling radial;
odd sizes; pyramid levels 0 to 3 and a half-size decode; mixed-sign Dehaze on
a brush; the curve-input probe; the camera-native fixture.

### 4.4 Before step 5.3

In steps 5.1 and 5.2 a GPU render of a plan with a brush throws
`std::runtime_error("brush masks are not rendered on the GPU yet")`. It is
thrown in `pointwiseOnGpu`, before `packPointwise`, because every GPU entry
reaches it: three `developOnGpu`, `resumeOrDevelopOnGpu` and two
`sampleOnGpu`. The
window, the export queue and the CLI's `--device auto` already fall back to
the CPU on a GPU failure and say so (`--device gpu` fails, as it does for any
GPU error); no reader or GUI action creates a brush before step 5.4 anyway.

## 5. Persistence

### 5.1 State JSON (`include/SettingsJson.h`, `src/core/SettingsJson.cpp`)

Brush geometry, as ADR 044 §9:

```json
{"id": 5, "type": "brush", "name": "", "enabled": true, "opacity": 1, "invert": false,
 "geometry": {"rasteriser": 1, "strokes": [
   {"radius": 0.02, "hardness": 0.5, "flow": 1, "erase": false,
    "points": [[0.1, 0.2], [0.11, 0.21]]}]},
 "deltas": {"exposure": 0.5}}
```

Numbers in shortest decimal. `"strokes": []` for an empty brush.

`WrittenMask` (`LocalAdjustmentCodec.h`), whose fields are flat scalars,
gains `std::shared_ptr<const StrokeList> strokes` (null for the other kinds;
`rasteriser` stays a scalar geometry field). Each format spells it: JSON as
the nested array above, XMP as below.
`localAdjustmentsToJson` gains a parameter `StrokeDetail detail =
StrokeDetail::Points`; `StrokeDetail::Digest` writes `"strokes": {"count": n,
"hash": "<16 hex digits of contentHash>"}` and is for keys only (never read
back).

### 5.2 XMP (`src/core/Sidecar.cpp`)

In the mask's structure: `arraw:rasteriser` (an integer) and

```xml
<arraw:strokes><rdf:Seq><rdf:li>(base64 of stroke 1)</rdf:li>…</rdf:Seq></arraw:strokes>
```

one `strokeBase64` string per stroke (format 1, as ADR 044 §9). An empty brush
writes an empty `rdf:Seq`. `localAdjustmentsVersion` stays 1 (decision 4a).
The mask-structure reader marks every nested element as `structured` today,
so `arraw:strokes` needs its own branch. It finds the `rdf:Seq` under the
property in each RDF spelling the structure itself may take, and reads each
`rdf:li`'s text.

### 5.3 Reading (format-neutral, `LocalAdjustmentCodec`)

`MaskFields` gains

```cpp
/// @brief One stroke as a document gave it, decoded but not yet normalised.
struct StrokeFields {
    std::optional<DecodedStroke> decoded; ///< Empty when it could not be decoded.
    std::string problem;                  ///< Why not ("not base64", "no points", ...).
    std::vector<std::string> unknownKeys; ///< JSON only: keys a stroke object should not have.
};
std::optional<std::vector<StrokeFields>> strokes; ///< Present when the document had the field.
```

and `rasteriser` is a geometry key (`partOfKey`). Each format decodes (XMP:
`strokeFromBase64(text, maximumStrokePoints)`; JSON: the object's fields, the
points cut at `maximumStrokePoints` and counted), and `readLocalAdjustments`
applies, for a mask of type `brush`, in this order:

1. Rasteriser absent or not an integer: the mask is dropped (malformed). Not
   `brushRasteriserVersion`: dropped, "unknown rasteriser N"
   (`LocalAdjustmentDropped`).
2. `strokes` absent: an empty list, no warning (XMP tools may drop an empty
   container). Present but not a sequence of strokes: the mask is dropped
   (malformed).
3. A stroke that could not be decoded, has a non-finite number or no points:
   that stroke is dropped (decision 1a).
4. Points past 10 000 were cut by the decoder.
5. Every other number is clamped by `normalised(Stroke)` (radius to 0.0005 to
   1, hardness and flow to 0 to 1, positions to -2 to 3).
6. Strokes from the 2 001st stored one on are dropped, counted by position in
   the document (as masks past 16 are, ADR 044 note of 2026-10-09), even when an
   earlier one was dropped under rule 3.
7. Walking the kept strokes in order with `StrokeList::accepts`: from the first
   that would pass the points, swept-area or dab budget, it and every later one
   are dropped. (A stroke that alone passes a budget makes `normalised` throw;
   it is treated as this case.)
8. The list is built; the mask is otherwise read as today.

Warnings: one `Notice::BrushStrokesTrimmed` (new) per mask and per reason,
with the mask's position and id, the count and the reason ("3 strokes could
not be read", "points past 10 000 cut from 2 strokes", "strokes from 1 801 on
dropped: past the swept-area budget", "radius clamped in 3 strokes"). Clamps
also go through `BrushStrokesTrimmed`, one per mask and field (radius,
hardness, flow, positions), because `SettingClamped` words one value and its
replacement and has no place for a count.
Unknown stroke keys: one `LocalAdjustmentFieldIgnored` per mask and key. A
2 000-stroke mask never makes 2 000 warnings.

`Diagnostics.cpp` (`describe`), `src/cli/StreamDiagnostics.cpp` and
`src/app/DebugLog.cpp` word the new notice.

### 5.4 Rasteriser versions

`brushRasteriserVersion` is 1 and the writer writes the list's own version.
`validate` and `normalised` refuse others, readers drop the mask. A future
rasteriser 2 must come with `localAdjustmentsVersion` 2, so that this build
reads such a list "as far as it is understood" and then refuses to write the
sidecar back (ADR 044 §9), instead of dropping the mask and saving. Recorded
in the ADR note (step 5.5).

### 5.5 Never copied

`withLook`, copy sections and presets leave masks alone (unchanged).

## 6. Python and the command line

### 6.1 Types (`src/python/BindSettings.cpp`)

- `Stroke` (frozen): `radius=0.02`, `hardness=0.5`, `flow=1.0`, `erase=False`,
  `points` a tuple of `(u, v)` tuples (accepts any sequence of pairs). Numbers
  as given; clamped when made into a `BrushMask` or added to a mask.
- `BrushMask` (frozen): `strokes` a tuple of `Stroke` (read as a tuple; built
  from any sequence), `rasteriser` read-only. Constructing normalises each
  stroke (clamps, as `add_brush_mask` does and as the other shapes are clamped
  when added), then builds the list. `StrokeList`'s own constructor refuses
  out-of-range values rather than clamping them. Constructing raises
  `ValueError` for a non-finite number, a stroke without points, and a cap or
  budget passed. Equality is `BrushMask`'s. `repr` is a summary
  (`BrushMask(12 strokes, 3456 points)`), never the points.
- `LocalAdjustment.shape` is `LinearMask | RadialMask | BrushMask`.

### 6.2 Photo methods (`src/python/BindPhoto.cpp`)

- `add_brush_mask(strokes: Sequence[Stroke] = (), *, name='', opacity=1.0,
  invert=False, enabled=True, **deltas) -> Photo`: each stroke normalised
  (clamped), then the list built; past a cap or budget, a full list (16) or a
  non-finite number is `ValueError`, an unknown keyword `TypeError`.
- `with_brush_stroke(id: int, stroke: Stroke) -> Photo`: `withStrokeAppended`
  (decision 5a).
- `with_local_adjustment(id, shape=BrushMask(...))` replaces the strokes.
- Rendering releases the GIL as today. Direct renders go through the shared
  cache up to `retainedCoveragePixels`, so repeated preview-sized renders in a
  loop rasterise once; above that they rasterise in bands without retention
  (§2.5).
- `.pyi` regenerated (`stubs.pattern`, `tests/python/test_stubs.py`).

### 6.3 `info` (`src/cli/InfoCommand.cpp`)

Text: `Brush 1 (brush): enabled, opacity 1, 12 strokes, 3456 points, exposure
+0.5` (the strokes and points between the opacity and the deltas, with the
existing number formatting). `--json`: the geometry as `stateToJson` writes it,
strokes included (ADR 044 §10; decision 7a).

`export` renders brushes from the sidecar on either device with no new flags.

## 7. GUI minimum (no creation, no painting)

- **List:** a brush mask from a sidecar is listed as "Brush 1" (or its name),
  with its check box; `KindRole` 3. Rename, enable, invert, duplicate, delete,
  reorder and the opacity and delta rows work as for the other kinds (all
  core edits).
- **Creation:** the Masks tab keeps only Linear and Radial. Nothing creates a
  brush.
- **History:** "Add Brush 1", "Remove Brush 1", "Brush 1: Exposure +0.50",
  "Paint Brush 1" for a strokes change (`MaskChange::strokes`; tested now,
  reached by the window in step 6), "Edit Brush 1" for several aspects.
- **Overlay:** a selected brush has no handles; brushes have no pins (selection
  is by the list). A press or drag on the photograph with a brush selected
  does what it does on empty canvas (pan, or clear the selection on a click).
  `pinPosition` becomes `std::optional<QPointF>`; `handlePositions` returns
  none and `handleAt` gives `MaskHandle::None` for a brush.
- **Tint (O):** drawn for a selected brush as for the other kinds, from
  `maskCoverage` (decision 6b, §7.1). Step 5.1 to 5.3 builds draw none (the
  refusal is caught, as any coverage failure is today); step 5.4 draws it.
- **Rendering:** the window renders the brush through its ladders (§2.6) on
  either device, level 1 first when level 0 would draw coverage from nothing
  (§2.10).

### 7.1 `maskCoverage` for a brush (decision 6b, step 5.4)

`maskCoverage(adjustment, frame, region, size)` (`include/DevelopedFrame.h`,
`src/core/DevelopedFrame.cpp`) supports a brush; linear and radial are
unchanged.

- **Level.** The grid's cell, in decoded-source pixels, is the shorter of the
  across and down steps the function already computes. The level is the
  largest `ℓ ≥ 0` with `2^ℓ` no more than that: a level whose pixel is no
  larger than a cell ("no coarser than the grid"). Its size is the decoded
  size (`frame.source().size`, which is the window's level 0) ceil-halved `ℓ`
  times, as `halved` makes the window's pyramid, so it is a size the window's
  renders put in the cache.
- **Coverage.** For `ℓ' = ℓ, ℓ - 1, …, 0`, `brushCoverageCache().find(strokes,
  size(ℓ'))`; the first hit is used. The window renders at device pixels and
  the tint's grid is at most half the widget's logical pixels, so the window's
  own level is at or below `ℓ` and this is usually a hit. With no hit,
  `brushCoverageCache().coverage(strokes, size(ℓ))` draws it on the calling
  (GUI) thread and keeps it; `retainedCoveragePixels` does not apply, since the
  size follows the view as the window's renders do.
- **Sampling.** Each cell centre, mapped to the corrected frame as for the
  other kinds, gives `(u, v)`; the value is the float coverage `m` of the pixel
  `(floor(u · w), floor(v · h))` of the chosen size, 0 off the raster (a brush
  paints only the photograph). The cell's weight is `round(255 · m)`, or
  `round(255 · (1 − m))` when inverted. Float coverage, not the dithered codes:
  the tint shows the mask, not a render. An empty brush gives all 0 (all 255
  inverted) and draws nothing.
- **Errors.** As today for the grid and region; an unknown rasteriser throws
  (`normalised`).
- **Threads.** The cache is thread-safe. A GUI-thread miss and a worker miss
  of the same strokes and size at once both draw; the cache keeps one entry,
  and the bits are equal.
- **Cost.** A hit samples at most `coverageCells` (about 1 M) cells: a few ms.
  A miss draws one brush at level `ℓ`: tens of ms at a fit view; at 1:1 on a
  24 MP photograph about 0.2 s for a realistic brush and about a second for
  one at the budget's worst (level 1 or 2). Zoomed past 1:1 a miss is at level
  0 (up to 0.8 s realistic, 4 s at the budget's worst), but the window's own
  level-0 render has usually filled the cache by then. The overlay's existing
  `mask.coverage` timing span logs it. This is heavy work on the GUI thread
  that ADR 043's spirit avoids; the user accepted it (decision 6b), and step 6
  may move it off the thread with the live stroke.
- **Overlay.** `MaskOverlay` calls it for a brush as for the other kinds; the
  "reduced grid during a gesture" latch does not arise (a brush has no drag in
  step 5). No status-line hint.

## 8. Tests

`[brush]` tags; slow cases (a second or more) also `[slow]`. Existing
prototype tests stay (`test_BrushStrokes`, `test_BrushRaster`,
`test_BrushCoverageCache`, `test_StrokeCodec`, the bench).

### 8.1 The plan's three named tests (render level, both backends)

1. **Same path at different event rates.** One stroke along a horizontal
   path, given with its corners only and with every segment split at its
   midpoint once, twice and three times (2×, 4×, 8× the events). The developed
   renders (export, and a ladder render at level 1) are bit-identical, on the
   CPU and on the GPU. Bit identity needs every division and product in
   `dabCentres` to be exact, which dyadic corners alone do not give (a segment
   of 3/64 divides inexactly). As in the prototype's exact test, therefore:
   every segment length and the radius are powers of two (corners such as
   0.25, 0.5, 0.75 at `v = 0.5`, radius 0.0625); the source's width is its long
   edge; and both sides are even, so that level 1 is an exact half. A diagonal
   path at 1× and 3× (splits not exact): the
   packed codes differ by at most 1, at no more than 1% of the pixels the
   stroke reaches (coverage within 1e-5 moves a code only near a threshold),
   and pixels with equal codes are bit-identical.
2. **Different resolutions.** (a) The float coverage the cache makes at the
   level 0 and level 1 sizes of an **even-sized** source (both sides even, so
   level 1 is an exact half) agree after box-halving within ADR 044 §6 (max
   0.021, mean 0.00015; mean 0.005 at hardness 1; the prototype measured a max
   of 0.02 on its seeds and 0.0201 on another, so the bound has that margin). Pyramid levels halve with
   `ceil`. For an odd side, level 1 is not an exact half and positions drift by
   up to half a pixel, which alone passes max 0.02 at hardness 1. Odd sizes are
   covered by (b) and by the parity cases.
   (b) A large hardness-1 stroke with Exposure +1: at levels 0, 1 and 2, and in
   a half-size decode, pixels deep inside it (code 255) equal the global render
   of the same level at exposure `g + 1` within step 2's full-weight tolerance,
   and pixels outside its reach equal the render without masks bit for bit,
   on both backends.
3. **After reload.** A state with three brushes (paint, erase, inverted) and a
   radial: through state JSON and through the XMP sidecar it reads back equal
   (`==`), and its render digest is bit-identical before and after, CPU and
   GPU; also through Python (§8.7).

### 8.2 Model and edits (`tests/test_BrushMask.cpp`)

Brush equality by pointer and by contents; a copied state is not an edit;
adding a brush (empty, and with strokes); `withStrokeAppended` clamps, refuses
an unknown id, a non-brush id, a non-finite stroke, and a stroke past each cap
and budget, leaving the state equal; `canAppendStroke` agrees with it;
duplicate shares the list; `validate` refuses a null list and rasteriser 2;
`maskTypeName`, `defaultMaskName` ("Brush 1", ordinals per kind),
`describeChange` (`strokes` set, `shape` not); a 17th add throws.

### 8.3 Plan and coverage (`tests/test_LocalPlan.cpp`, `tests/test_BrushCoverage.cpp`)

- Slots dense in plan order, skipping disabled and all-zero brushes; phase is
  the list index; two plans from equal states with different pointers are
  equal; a delta edit leaves every `BrushCoverageRef` equal and resumes from
  the Denoise checkpoint without rerunning noise reduction; an appended stroke
  invalidates Pointwise, not Denoise; Saturation-only brush edits keep
  `sameAtTap`, pre-tap ones do not.
- Quantiser: a golden table of `code` over all 16 phases and a set of `m`;
  `m = 0` and `m = 1` exact; 16 distinct phase shifts.
- Banded raster equals `quantise(rasteriseBrush)` bit for bit for 1, 2 and 8
  threads, bucket heights 1, 64 and 1000, observed and unobserved.
- Cache `find` returns hits only and never inserts; serials are unique and
  kept by untouched tiles of an extension; `drawn` with the bucket index
  equals `rasteriseBrush` bit for bit.
- Retention bound: a direct render at most `retainedCoveragePixels` leaves an
  entry in the cache and a second one makes no raster (counter); one above it
  leaves the cache's entry count and bytes unchanged; both give the same bits.
- Residency: a delta-only edit makes no cache call and no packing (counters);
  an appended stroke repacks exactly the tiles its dabs touch; undo repacks
  only those again; a level change repacks all; a cache budget of one tile
  still gives the direct path's bits; a cancelled update then a full one give
  the direct path's bits; a copied ladder has no residency.
- Progress weights: a delta-only edit through a ladder whose residency holds
  every brush gives the Coverage step zero weight; a cache hit gives only the
  packing weight.
- `peek` gives what `coverage` then gives (`Hit`, `ContentHit`, `Extended`,
  `Miss`) and changes neither the entries, the bytes nor the LRU order.
  `drawsBrushCoverage` is false with no brush, after a ladder render of the
  same state, after a delta-only edit, for an appended stroke (`Extended`) and
  when the cache holds the list behind another pointer; it is true for a new
  list at a size not cached, and after an eviction (step 5.2).
- Two threads, a ladder render and a direct render of the same strokes at
  once, give their single-threaded bits (run under the sanitiser builds).

### 8.4 CPU rendering (`tests/test_BrushRender.cpp`, `tests/test_RenderDigest.cpp`)

Outside every stroke a brush render is bit-identical to the render without
masks; an inverted empty brush equals the global render at `g + k`; a brush, a
linear and a radial sum as the ADR says; a ladder render equals a direct
render bit for bit (CPU); a region render is the crop of the whole; a
cancelled render leaves its rungs and the cache consistent and the next render
correct; the Coverage step is reported and the fraction never decreases; a
thumbnail-sized `develop` and `sample` with brushes work; digest states
`mask-brush-tone` (one soft brush, tone deltas) and `mask-brush-mixed` (paint,
erase, an inverted brush, a radial, Presence and Temp/Tint deltas), appended
after the existing mask states.

### 8.5 GPU (`tests/gpu/test_GpuBrush.cpp`, `test_GpuShaderLayout.cpp`, `test_GpuRenderDigest.cpp`, `test_GpuPlan.cpp`)

Header words for a brush; the reflection test sees bindings 8 to 11 and
eleven inputs; a coverage texture reads back equal to its host plane;
`updateCoverage` of some rectangles equals a whole upload; the parity cases of
§4.3; a GPU ladder render equals a direct GPU render, and an appended stroke
uploads only the changed tiles (an upload counter on the residency); a render
cancelled between the residency's update and its upload, then an uncancelled
one, equals a direct GPU render; a stroke edit rendered on the CPU through a
ladder, then a GPU render through the same ladder, equals a direct GPU render
(pending rectangles, §2.6); the coverage texture is not sRGB (texel readback
equals the host plane); GPU digests for the two new states. Run with
`just test-lavapipe` in the sandbox.

### 8.6 Persistence (`tests/test_LocalAdjustmentsJson.cpp`, `tests/test_LocalAdjustmentsSidecar.cpp`, `tests/test_ThumbnailCache.cpp`)

JSON and XMP round trips of empty, painted, erased, inverted and disabled
brushes and of names, reading back equal; the XMP bytes of a stroke are
`strokeBase64`'s; each reading rule of §5.3 with its warning and count
(including no more than one warning per mask and reason, stroke clamps
through `BrushStrokesTrimmed`, and the 2 000 cap counted by stored position
after an unreadable stroke); rasteriser 2 drops
the mask; absent `strokes` is an empty brush; the mask structure in the
alternative RDF spellings; a written sidecar with brushes and radials reads
back equal; thumbnail keys equal for equal strokes behind other pointers,
different for other strokes, unchanged for states without brushes.

### 8.7 Python (`tests/python/test_brush.py`, `test_cli_parity.py`, `test_stubs.py`)

`Stroke` and `BrushMask` construction (clamping out-of-range numbers, refusing
non-finite ones and budgets), freezing, equality and a short `repr`; `add_brush_mask`
clamps, refuses past a budget and at 16; `with_brush_stroke`;
`with_local_adjustment(shape=BrushMask(...))` and the kind check; JSON and
sidecar round trips equal; a render equals the C++ digest; stubs current; a
CLI/Python parity test over a sidecar with brushes.

### 8.8 CLI and GUI (`tests/test_Cli.cpp`, `tests/test_MasksModel.cpp`, `tests/test_HistoryModel.cpp`, `tests/test_MaskEditing.cpp`, `tests/test_MaskOverlay.cpp`, `tests/test_MainWindowMasks.cpp`)

`info` text and `--json` for a brush; `export` of a brush sidecar equals the
library's render; the list shows "Brush 1" with kind 3; history wording of
§7; no handles or pins for a brush and no crash on press, drag or paint with
one selected; opening a sidecar with a brush lists and renders it; the Masks
tab offers no brush creation.

### 8.9 The tint and the coarser first render (`tests/test_DevelopedFrame.cpp`, `tests/test_MaskOverlay.cpp`, `tests/test_PreviewRenderer.cpp`, `tests/test_MainWindowView.cpp`)

- **Tint (6b, step 5.4).** `maskCoverage` of a brush: cells deep inside a
  hardness-1 stroke are 255 and cells beyond its reach 0, inverted the
  complement; cells off the picture are 0 (255 inverted); an empty brush is
  all 0 and draws nothing (cache entry count unchanged); the level chosen is
  the coarsest no coarser than the grid (grids of several densities over one
  region); with the window's level in the cache it draws nothing (a raster
  counter) and uses that level even when finer than the grid's; with nothing
  cached it draws at the grid's level and a second call draws nothing; the same
  strokes behind another pointer hit; invalid grids and regions throw as
  today. `MaskOverlay` with O on and a brush selected paints a tint image,
  and none when O is off.
- **Coarser first render (9b, step 5.2).** `PreviewRenderer` given a level-0
  view of a state with a brush not yet cached delivers two results for the
  request, the first with `level` 1 and `provisional` set, the second with
  `level` 0 and not provisional, whose image equals a direct level-0 render;
  the same request again (cache and residency warm), a delta-only edit, and a
  view at level 1 or coarser deliver one result; a state without brushes
  delivers one; the background and the curve histogram never make a
  provisional render; a newer request queued during the provisional render
  cancels it and nothing provisional is delivered for the old request; a
  source without a level 1 (long edge under 512) renders level 0 only. On
  the GPU (step 5.3, lavapipe) the same sequence, both images from the GPU.
  `MainWindow` shows the provisional image, keeps the render indicator busy
  until the level-0 result, and takes no thumbnail from it.

## 9. Sub-steps

Each is separately reviewable and committable, leaves `just test` (and the
GPU tests under lavapipe, and `just py-test`) green, and leaves no user path
that creates a brush the export ignores: until step 5.4 no reader or GUI
action creates one, and a state built by hand in Python renders on the CPU
while the GPU and the sidecar writer refuse it with an error.

### Step 5.1: Model, edit rules, plan and the CPU direct path

- Files: `include/BrushStrokes.h` (moved), `include/LocalAdjustments.h`,
  `include/LocalAdjustmentEdits.h`, `include/EditSession.h`,
  `src/core/BrushStrokes.cpp`, `LocalAdjustments.cpp`,
  `LocalAdjustmentEdits.cpp`, `DevelopState.cpp`, `EditSession.cpp`,
  `LocalPlan.{h,cpp}`, `PointwisePlan.h`, `Develop.cpp`, new
  `BrushCoverage.{h,cpp}` (quantiser, `PackedCoverage`, bucket index, banded
  `packCoverage`, the process-wide cache and the retention bound of §2.5
  step 0), `BrushCoverageCache.{h,cpp}` (serial, `find`, the bucketed
  `drawn`), the private headers' includes of `<BrushStrokes.h>`,
  `DevelopedFrame.cpp`, `SettingsJson.cpp` and `LocalAdjustmentCodec.cpp`
  (the `logic_error` guard), `src/gpu/GpuDevelop.cpp` (the refusal, in
  `pointwiseOnGpu`), `ProcessingPlan`/`LocalPlan.cpp` (`PreTapMask`, §3.2), the app
  sites of §1.6 (`MasksModel`, `MaskPresentation`, `MaskEditing`,
  `MaskOverlay`, `HistoryModel`), the Python types of §6.1 (`BindSettings.cpp`,
  `_arraw.pyi`, `stubs.pattern`, `__init__.py`), `CMakeLists.txt`, tests §8.2, the plan part of
  §8.3, the quantiser and banded parts of §8.3, §8.4 without the ladder
  cases, named tests 1 and 2b on the CPU direct path.
- Ladder renders with a brush use the direct path for now (correct; retained
  in the cache up to the bound, not packed in the ladder).
- Done when: brushes built in C++ render on the CPU through every entry point,
  bit-identical outside strokes; the GPU refuses them and the window falls
  back; every variant site handles three kinds; the suite is green.

### Step 5.2: Retained coverage, progress and the coarser first render

- Files: `include/CheckpointLadder.h` (the private member),
  `src/core/LadderAccess.h`, `CheckpointLadder.cpp`, `BrushCoverage.{h,cpp}`
  (`CoverageResidency`, pending upload lists), `Develop.cpp` (`CpuStages`),
  `include/Progress.h` (`Coverage`), `RenderProgress.cpp` (weights of brushes
  not ready only, §2.8), `StageTable.h` (only
  if a check needs it), `src/app/RenderActivity.cpp`, tests: the residency and
  `find` parts of §8.3, the ladder, cancellation and progress cases of §8.4,
  named test 2a, the progress tests (`test_Progress`, `test_RenderActivity`,
  `test_RenderProgressPie`).
- Decision 9b (§2.10): `BrushCoverageCache::peek`, `include/Develop.h`
  (`drawsBrushCoverage`, sharing the per-brush test with the progress
  weights), `src/app/PreviewRenderer.{h,cpp}` (`PreviewResult::provisional`,
  the level-1 direct render before a level-0 `Layer::Shown` render, on the
  CPU and through `GpuPreview` with the uploaded level 1),
  `src/app/ui/MainWindow.cpp` (no thumbnail, no background, indicator kept
  busy for a provisional result); tests: the `peek` and `drawsBrushCoverage`
  parts of §8.3, the CPU part of §8.9's coarser first render. Until step 5.3
  the GPU refuses a brush, so both renders fall back to the CPU, as any
  brush render does then.
- Measure `coverageCost` and `packCost` (release build) and record them in
  `RenderProgress.cpp` as the other constants are. Measure a cache miss of
  B2's 16 masks at 24 MP through `drawn` beside the banded path.
- ADR 044 gets its note line for serials replacing "compare tile pointers"
  (the amendment's last bullet) in this commit, with the code that departs.
- Done when: a delta drag on a brush through a ladder does no coverage work
  (counter test), an appended stroke repacks only touched tiles, ladder and
  direct renders agree bit for bit, the Coverage step is reported, cancellation
  is clean; and (9b) a level-0 window render whose brush coverage is not ready
  delivers a provisional level-1 result first and then the level-0 result, a
  warm cache, a delta edit, an appended stroke or a level-1 view deliver one
  result, a newer request cancels the provisional render cleanly, and the
  window shows the provisional image without taking a thumbnail from it.

### Step 5.3: The GPU

- Files: `src/core/device/DeviceImage.h` (format note), `src/gpu/GpuContext.{h,cpp}`
  (`uploadCoverage`, `updateCoverage`, eleven inputs, `RhiDeviceImage` taking
  its format, RGBA8 readback), new
  `src/gpu/GpuCoverage.{h,cpp}` (the `DeviceCoverage` implementation),
  `GpuPlan.{h,cpp}` (header words), `GpuDevelop.cpp` (coverage into the pass,
  ladder residency, refusal removed), `shaders/develop.frag`, tests §8.5,
  named tests 1 and 2b on the GPU, the GPU case of §8.9's coarser first render.
- Done when: every parity case of §4.3 passes under lavapipe, texels read back
  exactly, partial uploads match whole ones, GPU digests are pinned, the window
  renders a brush on the GPU without falling back, the provisional level-1
  render included.
- To do here from 5.2: the stand-in skips the GPU until the GPU draws brushes
  (`gpuDrawsBrushes` in `PreviewRenderer.cpp` becomes true), and the "CPU and
  GPU ladder both say it draws" test in `render` becomes the GPU ladder's
  answer alone. A GPU ladder's residency may exist without a rung:
  `drawsBrushCoverage` already answers false when a usable rung at the
  pointwise pass or beyond is held, but check it for the GPU's rungs.

### Step 5.4: Persistence, the GUI minimum and `info`

- Files: `include/Diagnostics.h` (`BrushStrokesTrimmed`),
  `src/core/Diagnostics.cpp`, `include/SettingsJson.h` (`StrokeDetail`),
  `src/core/SettingsJson.cpp`, `LocalAdjustmentCodec.{h,cpp}` (guard removed,
  `WrittenMask::strokes`, §5.3), `Sidecar.cpp` (the `arraw:strokes` branch),
  `src/app/ThumbnailCache.cpp`, `DebugLog.cpp`,
  `src/cli/InfoCommand.cpp`, `StreamDiagnostics.cpp`, the GUI behaviour of §7,
  tests §8.6, §8.8, named test 3 (C++).
- Decision 6b (§7.1): `include/DevelopedFrame.h` (the brief and `@throws` of
  `maskCoverage` say a brush is supported), `src/core/DevelopedFrame.cpp` (the
  refusal replaced by the level choice, the cache lookups and the sampling),
  `src/app/ui/MaskOverlay.cpp` (nothing brush-specific left to skip); tests:
  the tint part of §8.9.
- From here a sidecar can carry a brush, and both backends render it.
- Done when: all round trips are exact, every reading rule warns once per mask
  and reason, the window lists and renders a loaded brush and offers none, and
  `info` describes it; and (6b) O tints a selected brush, `maskCoverage` of a
  brush matches §8.9 (levels, hits without drawing, inversion, off-picture
  cells, empty brush), and the `mask.coverage` timing of a cached brush at a
  fit view is a few ms (logged, not asserted).

### Step 5.5: Python, parity and the documents

- Files: `src/python/BindPhoto.cpp` (the methods of §6.2), `PyBindings.h`,
  `arraw/_arraw.pyi`, `arraw/__init__.py`,
  `tests/python/test_brush.py`, `test_cli_parity.py`; ADR 044 gets a note "what
  step 5 settled" (dither phase, residency in the ladder, serials, the bucketed
  banded raster, the Coverage step, the rasteriser/version rule, the reading
  rules, the decisions below, among them the brush tint drawn on the GUI
  thread (6b, an accepted exception to keeping heavy work off it) and the
  coarser first render (9b)); `docs/todo.md` and
  `local-adjustment-plan.md` mark step 5 done.
- Done when: `just py-test` is green with the stubs current, Python-made
  brushes render and round-trip, and the CLI/Python parity test passes.

## Decisions (user, 2026-10-10)

The user answered the questions of the reviewed plan with the preferred option
everywhere except 6 and 9. The options not taken are kept in one line each.

1. **A stroke that cannot be read** (bad base64, a non-finite number, no
   points) in an otherwise good brush: **(a)** drop that stroke, keep the
   rest, warn (§5.3 rule 3). Not taken: (b) drop it and every later stroke;
   (c) drop the whole mask. (a) deviates from a literal reading of ADR 044 §9
   ("a malformed entry, a non-finite number" drop the entry); the step-5 ADR
   note records it.
2. **Progress while painting coverage:** **(a)** a new
   `ProgressStep::Coverage`, "Painting brush masks…" (§2.8). Not taken: (b)
   folding it into `Context`.
3. **Dither phase of a mask:** **(a)** its index in the state's list (§2.4).
   Not taken: (b) its slot among the plan's brushes; (c) its id modulo 16.
4. **The list version for brushes:** **(a)** `localAdjustmentsVersion` stays 1;
   the builds of steps 2 and 3 were never released (§5.2). Not taken: (b)
   version 2 when the list holds a brush.
5. **Appending a stroke from Python:** **(a)** `photo.with_brush_stroke(id,
   stroke)` beside `add_brush_mask` (§6.2). Not taken: (b) only
   `add_brush_mask` and `with_local_adjustment(id, shape=…)`.
6. **The red tint of a selected brush mask (O):** **(b)** now: `maskCoverage`
   supports a brush through the shared cache at a pyramid size no coarser than
   the grid, on the GUI thread (usually a cache hit, worst case about a
   second). Design §7.1, tests §8.9, step 5.4. Not taken: (a) no tint in step
   5, drawn off the GUI thread in step 6.
7. **`info --json` for a brush:** **(a)** the full strokes, as `stateToJson`
   writes them (§6.3). Not taken: (b) a summary with full strokes behind a
   flag.
8. **Window memory at a 1:1 view with many brushes:** **(a)** accepted: the
   cache (up to 512 MiB), the ladder's host planes and, on the GPU, the
   textures (366 MiB each for 16 masks at 24 MP). Not taken: (b) no host
   planes on a GPU ladder.
9. **Opening at full size:** **(b)** when a level-0 window render misses the
   coverage cache, the window first renders one level coarser and shows it,
   then renders level 0. "Misses" is made precise as "would draw a brush's
   coverage from nothing" (neither in the ladder's residency nor in the cache,
   whole or as a list it extends), so that delta drags, undo and step 6's
   strokes never pay a second render. Design §2.10, tests §8.3 and §8.9, step
   5.2 (CPU) and 5.3 (GPU). Not taken: (a) accept the wait and measure in step
   6; (c) prefetch full-size coverage in the background.
