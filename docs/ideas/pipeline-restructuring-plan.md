# Pipeline restructuring: pointwise sub-blocks, one stage table, a checkpoint ladder — plan

Status: proposed, 2026-10-08. Step 1 of `local-adjustment-plan.md`
("Restructuring, no output change"), which is the restructuring of step (c) in
`architecture-roadmap.md`. It prepares what ADR 044 builds in step 2 and
changes no pixel.

## Goal

Three changes, in two sub-steps, each leaving the build green and every output
bit-identical:

- **A.** `PointwisePlan` and `TonePlan` come out of the flat `ProcessingPlan`.
  The per-pixel chain (CPU `developPixel`, `develop.frag`) takes sub-blocks,
  and the values ADR 044 makes per-pixel pass through one seam
  (`PixelAmounts`). The tone resolution (settings to coefficients) becomes
  small `noexcept` functions that a pixel can call.
- **B.** One stage table and one generic driver serve both backends. The
  checkpoints the preview keeps by hand move into an engine-owned
  `CheckpointLadder`.

What step 2 then only has to add: `LocalPlan local` in `PointwisePlan`, a
per-pixel `PixelAmounts` in place of the global one, the GLSL mirrors of the
resolution functions, and two haze bases. No driver, checkpoint or ladder code
changes for masks: a mask edit changes the pointwise block, so it resumes from
the Denoise rung by the prefix rule (or from the source when noise reduction
is off, at the same cost).

## What exists today

### The plan and the chain

`ProcessingPlan` (`src/core/ProcessingPlan.h`) is flat in its pointwise part:
`toWorking`, `exposureGain`, `shapesTone`, `contrastSlope`, `contrastScale`,
four shifts, `shoulderKnee`, `presence`, `toneCurves`, `colorAdjustments` sit
beside the blocks `denoise`, `geometry`, `resize`, `effects`. `stagesOf` ties
those thirteen pointwise fields by hand. `tonePlanFor` returns a whole
`ProcessingPlan`. Every chain function (`shapeLuminance`, `shapeTone`,
`applyToneCurves`, `rollHighlights`, `developToCurveInput`, `developPixel`,
`developToTap`) takes the whole plan, and so does `curveInputFieldsOf`. The
shader reads the uniform block `plan.*` directly in every function, so no
function there can take a per-pixel value. `packPointwise` reads the plan's
geometry block for the Presence grid sizes, so the pointwise pass reads a
later group.

### Two drivers

- **CPU** (`src/core/Develop.cpp`): `runStages` (four `if`s from Denoise to
  Effects), `develop`'s direct expression, `developUntil`'s Denoise special
  case (`denoisedCopy`), `resumeFrom`'s Denoise special case (borrow instead
  of clone), and `sample`'s `pointwiseFromSource`. The skips are inside the
  stage functions (`applyGeometry` and `resample` return an identity input,
  `effectsBy` checks `active()`).
- **GPU** (`src/gpu/GpuDevelop.cpp`): `runPasses`, already one loop with
  `done`, `stopAfter` and a probe, with the skips in the loop.

Both skip under the same conditions, written twice. `stepAfter` and
`stepThrough` (`RenderProgress.cpp`) and the size check in `staleReason`
(`RenderCheckpoint.cpp`) are three more per-stage switches.

### Who keeps which checkpoints, and why

| Holder | Checkpoints | Why | Dropped when |
|---|---|---|---|
| `PreviewRenderer`, CPU path, shown layer (`cpuCache`, a `CheckpointCache`) | Denoise (only while noise reduction is on), Pointwise, Geometry, Resize of the pyramid level last rendered; host | A tone edit with NR on resumes after Denoise; a geometry edit after Pointwise; a viewport change (zoom, pan, window) after Geometry; a vignette or grain edit, or the same request, after Resize (ADR 024, 039, 037) | Another level buffer (`bind`), a new source, no source, a rung the engine calls stale (`canResumeFrom`) |
| `PreviewRenderer`, CPU path, background layer (`backgroundCache`) | The same four, for the reduced whole frame beneath a region | So that the fallback's coarser level never evicts the shown layer's rungs | As above |
| `GpuPreview::checkpoints_` and `backgroundCheckpoints_` | The same four per layer, resident on the worker's device | As above; on the GPU a rung shares its device image, so keeping it is free | As above, and on a GPU failure (`fail`) or device loss; released on the worker thread, as the device requires |
| `GpuPreview::uploaded_` | Not checkpoints: each pyramid level uploaded once | The source rung of the GPU path: no transfer per render | New source, failure, loss |
| `ExportQueue` (`GpuExport::develop`), CLI (`developOnDevice`) | One Effects checkpoint, read back and dropped at once | `developOnGpu` returns one; nothing is reused | Immediately |
| `develop()` (CPU export, thumbnails, Python, CLI CPU path) | None | The direct path copies nothing | — |
| `sample`, `sampleOnGpu`, `CurveHistogramRefresh` | None (a tap is inside a pass); the refresh keeps a plan, not pixels | ADR 035 | — |

The Effects result is never kept: it is the render's return value. The
preview's chain of public calls (`developUntil` to the first rung, then a
`resumeFrom` per boundary) plans the render once per call and once more per
`canResumeFrom`, opens a progress root per call, and on the CPU clones each
rung it resumes from (ADR 024 Consequences). The rung policy, the
deepest-first search and the "stale versus bad request" rule live in a
template in the app (`CheckpointCache::render`) with three lambdas.

## Design

### 1. Plan types and headers

All private, in `src/core`. No public header changes in A.

| Type | Header | Contents |
|---|---|---|
| `TonePlan` | `TonePlan.h` (new) | `exposureGain`, `shapesTone`, `contrastSlope`, `contrastScale`, `shadowShift`, `highlightShift`, `blackShift`, `whiteShift`; defaulted `==`. Not the shoulder: the shoulder stays global under ADR 044 and acts after the curve-input tap |
| `ToneAmounts` | `TonePlan.h` | Basic Tone in setting units, finite and clamped: `exposure`, `contrast`, `highlights`, `shadows`, `whites`, `blacks` |
| `PresenceAmounts` | `Presence.h` | `texture`, `clarity`, `dehaze` (minus one to one); `PresencePlan::amounts` replaces the three floats |
| `ChromaAmounts` | `ColorAdjustments.h` | `adjustsSaturation`, `saturation`, `adjustsVibrance`, `vibrance`; `ColorAdjustmentPlan::chroma` replaces the four fields |
| `PointwisePlan` | `PointwisePlan.h` (new) | `toWorking`, `tone`, `presence`, `toneCurves`, `shoulderKnee`, `colorAdjustments`, in chain order; defaulted `==`. Step 2 adds `local` |
| `PixelAmounts` | `PointwisePlan.h` | `TonePlan tone`, `PresenceAmounts presence`, `ChromaAmounts chroma`: the values ADR 044 §2 makes per-pixel. Step 2 adds the Temp/Tint gain |
| `ProcessingPlan` | `ProcessingPlan.h` | `denoise`, `pointwise`, `geometry`, `resize`, `effects`: one member per `Stage`, nothing else |

Header contents after A:

- **`TonePlan.h`**: `toPerceptual`, `toLinear`, `greyPivot`, `smoothstep`, the
  four region weights, `liftedBlackThreshold`, `regionalReach`,
  `endpointReach` (out of `ToneSettings.cpp`'s anonymous namespace),
  `ToneAmounts`, `TonePlan`, the resolution functions (§2),
  `toneAmountsOf`, `tonePlanFor`, `shoulderKneeFor`, `shapeLuminance`,
  `shapeTone`, `rollHighlights`. `ToneSettings.cpp` is renamed
  `TonePlan.cpp`.
- **`PointwisePlan.h`**: `PointwisePlan`, `PixelAmounts`, `globalAmountsOf`,
  `curveRatioFloor`, `applyToneCurves`, `developToCurveInput`,
  `developPixel` (three overloads), `developToTap`, `curveInputFieldsOf`.
- **`ProcessingPlan.h`** keeps `PixelRegion`, `regionOf`, `ResizePlan`,
  `ProcessingPlan`, `stagesOf`, `frameMappingOf`, `prefixMatches`,
  `plannedRequest`, `colorMatrixFor`, the three `planFor`, `sameAtTap`,
  `toPerceptualSigned`, `fromPerceptualSigned`. It includes
  `PointwisePlan.h`, so every current includer still compiles unchanged.

### 2. Per-pixel resolution functions

ADR 044 resolves each control per pixel as `e = clamp(g + s)`, then "the same
expression the global setting uses". Today that expression is fused with
validation (throwing on NaN) inside `tonePlanFor`. A splits the two:

```cpp
// TonePlan.h: called by the planner today, by the chain per pixel in step 2.
// Each takes a finite setting already inside its range, and never throws.
[[nodiscard]] inline float exposureGainFor(float exposure) noexcept;      // std::exp2(exposure)
[[nodiscard]] inline float contrastSlopeFor(float contrast) noexcept;     // std::exp2(contrast / (2.0F * steepestContrast))
[[nodiscard]] inline float contrastScaleFor(float slope) noexcept;        // std::pow(greyPivot, 1.0F - slope)
[[nodiscard]] constexpr float regionalShiftFor(float setting) noexcept;   // regionalReach * setting / strongestToneControl
[[nodiscard]] constexpr float endpointShiftFor(float setting) noexcept;   // endpointReach * setting / strongestToneControl
[[nodiscard]] inline TonePlan resolveTone(const ToneAmounts& amounts) noexcept;

// Host only: refuses a non-finite setting with today's messages and order
// (exposure, contrast, shadows, highlights, blacks, whites), clamps the rest.
[[nodiscard]] ToneAmounts toneAmountsOf(const ToneSettings& settings);
[[nodiscard]] TonePlan tonePlanFor(const ToneSettings& settings);   // resolveTone(toneAmountsOf(settings))
[[nodiscard]] float shoulderKneeFor(const ToneSettings& settings);   // unchanged maths, now declared

// Presence.h and ColorAdjustments.h, likewise:
[[nodiscard]] constexpr float presenceAmountFor(float setting) noexcept;                         // setting / strongestPresence
[[nodiscard]] constexpr ChromaAmounts chromaAmountsFor(float saturation, float vibrance) noexcept; // != 0 flags, / strongestSaturation
```

Each body is today's expression character for character, so the planner's
results are bit-identical. `shapesTone` is computed from the clamped amounts;
clamping never changes whether a finite value is zero, so it is the same flag.
Individual functions, not only `resolveTone`, because step 2 resolves only the
controls a mask touches (ADR 044's `touched` bits; contrast's `exp2` and
`pow` only when contrast is touched).

The GLSL mirrors of these functions are **not** written in A: the shader
cannot call them without resolving on the device, which would change GPU
output. They arrive in step 2 with their parity tests (ADR 044 §7).

### 3. The chain takes sub-blocks

CPU (`PointwisePlan.h`, `TonePlan.h`, `Presence.h`, `ColorAdjustments.h`):

```cpp
Colour shapeTone(const TonePlan& tone, Colour colour);              // and shapeLuminance(const TonePlan&, float)
Colour rollHighlights(float shoulderKnee, Colour colour);
Colour applyToneCurves(const ToneCurvePlan& curves, Colour colour);
Colour applyPresence(const PresencePlan& plan, const PresenceAmounts& amounts, Colour colour,
                     float logLuminance, const PixelContext& context);
Colour adjustColor(const ColorAdjustmentPlan& plan, const ChromaAmounts& chroma, Colour colour);
// plus applyPresence(plan, colour, logLuminance, context) and adjustColor(plan, colour),
// which pass plan.amounts and plan.chroma: the global case, as tests and callers use it.

struct PixelAmounts { TonePlan tone; PresenceAmounts presence; ChromaAmounts chroma; };
[[nodiscard]] constexpr PixelAmounts globalAmountsOf(const PointwisePlan& plan) noexcept;

Colour developToCurveInput(const PointwisePlan& plan, const PixelAmounts& at, Colour colour,
                           const PixelContext& context);
Colour developPixel(const PointwisePlan& plan, const PixelAmounts& at, Colour colour,
                    const PixelContext& context);
Colour developPixel(const PointwisePlan& plan, Colour colour, const PixelContext& context); // globalAmountsOf(plan)
Colour developPixel(const PointwisePlan& plan, Colour colour);                              // asserts Presence off
Colour developToTap(const PointwisePlan& plan, Colour colour, Tap tap, const PixelContext& context);
```

`developToCurveInput` reads `plan.toWorking`, `at.tone` (exposure then
`shapeTone`), `plan.presence` with `at.presence`; `developPixel` then
`plan.toneCurves`, `plan.shoulderKnee`, `plan.colorAdjustments` with
`at.chroma`. The CPU traversal (`developSamples`) works out
`globalAmountsOf(plan)` once per render and passes it to every pixel; step 2
replaces exactly that with a per-pixel value.

GLSL (`develop.frag`), mirroring the same names line for line. The uniform
block, its layout and its bindings do not change; the structs are locals:

```glsl
struct TonePlan { float exposureGain; bool shapesTone; float contrastSlope; float contrastScale;
                  float shadowShift; float highlightShift; float blackShift; float whiteShift; };
struct PresenceAmounts { float texture; float clarity; float dehaze; };
struct ChromaAmounts { bool adjustsSaturation; float saturation; bool adjustsVibrance; float vibrance; };
struct PixelAmounts { TonePlan tone; PresenceAmounts presence; ChromaAmounts chroma; };
PixelAmounts globalAmounts();                       // from plan.*, the uniform block
float shapeLuminance(TonePlan tone, float luminance);
vec3 shapeTone(TonePlan tone, vec3 colour);
vec3 applyPresence(PresenceAmounts amounts, vec3 colour, float logLuminance, ivec2 at);
vec3 adjustColor(ChromaAmounts chroma, vec3 colour);
// main(): const PixelAmounts amounts = globalAmounts(); then the stages as today.
```

`applyPresence` keeps its present conditions (`plan.fineReduction != 0u`,
`amounts.clarity != 0.0`, `amounts.dehaze == 0.0`). Under masks they must read
base existence rather than amounts (ADR 044 §5); that is step 2's change,
since it needs new block flags.

### 4. `stagesOf`, `prefixMatches`, `curveInputFieldsOf`, `sameAtTap`

```cpp
[[nodiscard]] inline auto stagesOf(const ProcessingPlan& plan) {
    return std::tie(plan.denoise, plan.pointwise, plan.geometry, plan.resize, plan.effects);
}
[[nodiscard]] inline auto curveInputFieldsOf(const PointwisePlan& plan) {
    return std::tie(plan.toWorking, plan.tone, plan.presence);   // step 2 adds preTapLocalFieldsOf(plan.local)
}
// sameAtTap: denoise, geometry and resize equal, and
//            curveInputFieldsOf(first.pointwise) == curveInputFieldsOf(second.pointwise).
```

- `stagesOf` becomes ADR 011's intended shape: one block per stage, a flat
  `tie`. The `static_assert` on its size stays. A test pins
  `test::fieldCount<ProcessingPlan> == stageCount` (`tests/support/FieldCount.h`),
  which is the field-count guard ADR 011 and 015 wanted from reflection: a
  member added to the plan outside a stage block fails it.
- `prefixMatches` is unchanged: the fold compares `std::get<I>` of each tie,
  now whole blocks.
- `curveInputFieldsOf` compares the same values as today: `TonePlan`'s
  defaulted `==` covers exactly the eight tone fields it listed, and the
  shoulder stays out (it is after the tap). Tests pin
  `fieldCount<PointwisePlan> == 6` and `fieldCount<TonePlan> == 8`, so a field
  added to either fails until `curveInputFieldsOf` and this note have been
  looked at.
- `presenceContextFieldsOf` is unchanged (`lumaRow` and the bases; the
  amounts moved into `amounts`, which it never listed).

### 5. GPU packing

`packPointwise(const PointwisePlan& plan, ImageSize source, PointwiseProbe
probe = PointwiseProbe::Developed)`. It reads only its block and the size of
the pass's input, for the Presence grid sizes, instead of
`plan.geometry->sourceSize`. `GpuDevelop` passes `image.size()`, the same
value (today's `assert` says so), and the assert goes. `GpuPointwiseBlock`,
its offsets, the shader's block and `test_GpuShaderLayout` are untouched.

### 6. The stage table

One table, in two physical parts because core does not link the GPU.

The logical table:

| Stage | Plan block | Runs when (else the boundary collapses) | Pixel size at the boundary | Progress steps | CPU | GPU |
|---|---|---|---|---|---|---|
| Denoise | `denoise` | `denoise.active()`; collapsed, the boundary is the source itself | source | Denoise | `applyDenoise` | `denoiseOnGpu` |
| Pointwise | `pointwise` | always | source | Context, Pointwise | `runPointwise` (context, then the chain over row bands) | `pointwiseOnGpu` (curve table, Presence renders, the Pointwise pass) |
| Geometry | `geometry` | `!geometry->isIdentity()` | `geometry->outputSize` | Geometry | `applyGeometry` | `geometryOnGpu` |
| Resize | `resize` (and the geometry's output size) | `!resize->isIdentity(geometry->outputSize)` | `resize->outputSize` | Resize | `resizeBy` (cut, then `resample`) | `resizeOnGpu` |
| Effects | `effects` (and `frameMappingOf`) | `effects.active()` | `resize->outputSize` | Effects | `applyEffects` | `effectsOnGpu` |

The shared part, `src/core/StageTable.h` (private; arraw-gpu sees `src/core`):

```cpp
struct StageRow {
    Stage stage;                                         ///< Boundary the row ends at.
    ProgressStep firstStep;                              ///< First progress step the pass runs.
    ProgressStep lastStep;                               ///< Last progress step the pass runs.
    bool (*runs)(const ProcessingPlan& plan);            ///< Whether the pass runs, or its boundary collapses.
    ImageSize (*sizeAt)(const ProcessingPlan& plan, ImageSize source); ///< Size of the pixels at the boundary.
};
inline constexpr std::array<StageRow, stageCount> stageTable{ /* the five rows, captureless lambdas */ };
static_assert(/* row i has stage i */);
[[nodiscard]] constexpr const StageRow& rowOf(Stage stage) noexcept;
[[nodiscard]] constexpr Stage following(Stage stage) noexcept;          // precondition: not Effects
[[nodiscard]] bool keepsRung(Stage boundary, const ProcessingPlan& plan); // §8's policy
```

The plan block column is `stagesOf` itself, which already maps each `Stage`
index to its block; the table does not repeat it. `stepAfter` is the next row's
`firstStep` (Effects for the last), `stepThrough` is `lastStep`, and
`staleReason`'s expected size is `sizeAt`: three switches become table reads.

The backend columns are each one `switch (stage)` in the backend's `run`
(§7), one line per row calling the stage's function. `-Wall` warns about a
missing enumerator, so a new `Stage` without its case is visible on both
backends.

### 7. The driver

`src/core/StageDriver.h` (private), one function template over a backend.
The template is justified by the rule in CLAUDE.md: one loop for two pixel
types that cannot share a base (`ImageBuffer` is move-only and may be
borrowed; `DeviceImage` is a shared handle), against today's two loops and
five special cases.

```cpp
template <typename B>
concept StageBackend = requires(B& backend, typename B::Pixels pixels, const ProcessingPlan& plan,
                                Stage stage, const RenderCheckpoint& checkpoint) {
    { backend.run(stage, std::move(pixels), plan) } -> std::same_as<typename B::Pixels>;
    { backend.checkpoint(stage, std::move(pixels), plan) } -> std::same_as<RenderCheckpoint>;
    { backend.borrow(checkpoint) } -> std::same_as<typename B::Pixels>;
};

template <StageBackend B> struct StagesRun {
    typename B::Pixels pixels; ///< Pixels at the last boundary run.
    Stage done;                ///< That boundary.
};

/// Runs the passes after `done` (the source when empty) up to `stopAfter`,
/// skipping a pass whose row says it does not run. With a ladder, each
/// boundary before the last that keepsRung() names is stored as a rung and
/// the run carries on from the rung's pixels.
template <StageBackend B>
StagesRun<B> runStages(B& backend, std::optional<Stage> done, typename B::Pixels pixels,
                       const ProcessingPlan& plan, Stage stopAfter, CheckpointLadder* ladder = nullptr) {
    std::optional<Stage> at = done;
    while (!at || *at < stopAfter) {
        const Stage next = at ? following(*at) : Stage::Denoise;
        if (rowOf(next).runs(plan)) {
            pixels = backend.run(next, std::move(pixels), plan);
        }
        at = next;
        if (ladder != nullptr && next != stopAfter && keepsRung(next, plan)) {
            const RenderCheckpoint rung = backend.checkpoint(next, std::move(pixels), plan);
            LadderAccess::store(*ladder, rung);
            pixels = backend.borrow(rung);
        }
    }
    return {std::move(pixels), *at};
}
```

**CPU backend** (`Develop.cpp`, anonymous namespace):

- `HostPixels`: a borrowed `const ImageBuffer*` (the source, or a rung's
  immutable payload) or an owned `ImageBuffer`. `view()` reads either;
  `take()` gives an owned buffer, cloning a borrowed RGBA float one and
  converting any other layout with `toRgbaF32`. That is `denoisedCopy`'s rule,
  and the only borrowed non-float buffer is the source.
- `CpuStages { std::optional<Tap> tap; }`: `run` reads `view()` for Denoise and
  Pointwise (which only read) and `take()`s for Geometry, Resize and Effects
  (which consume by value). Pointwise runs `developPixel` with the global
  amounts, or `developToTap` when `tap` is set. `checkpoint` is
  `makeCheckpoint(stage, plan, take())`; `borrow` points into the
  checkpoint's buffer.
- So a collapsed Denoise reads the source without a copy, a resume from the
  Denoise rung reads it without a copy, and a resume from a later rung clones
  once, as today (ADR 024, 039). The clone count of every path is today's.

**GPU backend** (`GpuDevelop.cpp`): `GpuStages { GpuContext& context;
PointwiseProbe probe; }`, `Pixels = DeviceImage`. `run`'s switch calls
`denoiseOnGpu`, `pointwiseOnGpu` (extracted from `runPasses`: the curve table
upload when a curve is active and the probe reads curves, `presenceOnGpu`, the
Pointwise render), `geometryOnGpu`, `resizeOnGpu`, `effectsOnGpu`.
`checkpoint` shares the image; `borrow` returns the rung's image.

The progress spans and timing spans stay inside the stage functions, as
today, so the span order and unit counts do not change.

**Entry points**, each: validate, plan, open the `ProgressRoot`, call
`runStages`, finish:

| Entry | Start | `stopAfter` | Ladder | Result |
|---|---|---|---|---|
| `develop` | source (borrowed) | Effects | — | `take()` of the pixels: the Pointwise result onwards is owned, so no copy |
| `developUntil` | source | given (request planned by `plannedRequest`) | — | `checkpoint(done, …)`; at a collapsed Denoise, `take()` copies the source as today |
| `resumeFrom` | `borrow(from)` at `from`'s boundary | given | — | `from` itself when `stopAfter` is its boundary; else as above |
| `sample` | source, `CpuStages{tap}` | Resize | — | `encodeTap(view())` |
| `resumeOrDevelop` (new, §8) | deepest usable rung, else source | Effects | yes | `LadderRender` |
| `developOnGpu` (both overloads), its resume overload, `sampleOnGpu` | the same, with `GpuStages` | as today | — | as today |
| `resumeOrDevelopOnGpu` (new, §8) | deepest usable rung, else the upload | Effects | yes | `LadderRender` |

`requireBoundary`, `requireUploaded`, `requireResumable`, the residency checks
and every error message stay as they are.

### 8. `CheckpointLadder`

Public, because the preview (app) holds it and the render verbs that take it
sit beside `develop` and `developOnGpu`; ADR 018 does not require Python to
bind it. `include/CheckpointLadder.h`:

```cpp
/// @brief The checkpoints one caller keeps of one source, one per pass boundary.
///
/// A render through ::arraw::resumeOrDevelop (or resumeOrDevelopOnGpu) resumes
/// from the deepest rung its plan still matches, drops every rung that does
/// not, and stores a rung at each boundary it passes before the last. The
/// engine decides what is kept and what is stale (ADR 011); the caller only
/// holds the ladder and clears it when something the plan cannot see changes,
/// such as the device. Bound to one source buffer, which it keeps alive so that
/// its address identifies it (until the plan has ADR 012's decode block).
/// Not thread-safe. A ladder holding resident rungs belongs to its device's
/// thread: render through it, clear it and destroy it there.
class CheckpointLadder {
public:
    /// @brief Drops every rung and the source.
    void clear() noexcept;
    /// @brief Whether it holds no rung.
    [[nodiscard]] bool empty() const noexcept;
    /// @brief Whether it holds a rung at a boundary.
    [[nodiscard]] bool holds(Stage boundary) const noexcept;

private:
    friend struct LadderAccess;
    std::shared_ptr<const ImageBuffer> source_;
    std::array<std::optional<RenderCheckpoint>, stageCount> rungs_;
};

/// @brief A render through a ladder: its result, and where it resumed from.
struct LadderRender {
    RenderCheckpoint checkpoint;      ///< At ::arraw::Stage::Effects.
    std::optional<Stage> resumedFrom; ///< Boundary of the rung resumed from; empty from the source.
};
```

`include/Develop.h`:

```cpp
[[nodiscard]] LadderRender resumeOrDevelop(CheckpointLadder& ladder,
                                           std::shared_ptr<const ImageBuffer> source,
                                           const DevelopState& state, const RenderRequest& request = {},
                                           ProgressChannel* progress = nullptr);
```

`src/gpu/GpuDevelop.h`:

```cpp
[[nodiscard]] LadderRender resumeOrDevelopOnGpu(GpuContext& context, CheckpointLadder& ladder,
                                                std::shared_ptr<const ImageBuffer> source,
                                                const DeviceImage& uploaded, const DevelopState& state,
                                                const RenderRequest& request = {},
                                                ProgressChannel* progress = nullptr);
```

The engine side, `src/core/LadderAccess.h` (private), used by the two entries
and the driver:

- `bind(ladder, source)`: another source pointer clears the rungs first.
- `deepestUsable(ladder, plan, sourceSize, holdsHere)`: from Resize down to
  Denoise, drop a rung the backend cannot resume (`holdsHere`: a host buffer
  for the CPU, a device image of this context for the GPU) or that
  `staleReason` refuses; return the first that passes.
- `store(ladder, rung)`: put it at its boundary.

The rules, which are today's `CheckpointCache` rules moved into the engine:

- **Plan first.** The render is planned once, with the whole request, before
  the ladder is touched, so a bad request throws and drops nothing. (Today a
  request whose region is bad, on an empty cache, first stores the Pointwise
  and Geometry rungs and then fails; after B it stores none. The result is the
  same error.)
- **Deepest first.** As `CheckpointCache::render`: a deeper rung that is stale
  is dropped before a shallower one is tried. A rung's stored plan is the
  whole plan of the render that made it; only its prefix is compared.
- **Which rungs.** `keepsRung`: every boundary a render passes before its last,
  except Denoise when it collapsed (it would be a copy of the source the
  caller keeps anyway). A collapsed Geometry is kept, as today, because the
  preview reports "a viewport change resumes from the geometry" with no
  geometry (test_PreviewRenderer). The Effects result is returned, not kept.
- **Cancellation and failure.** A rung is stored only after its pass
  returned, so a cancelled or failed render leaves the rungs it finished and
  no partial one (ADR 042).
- **Progress.** One `ProgressRoot` from `stepAfter(resumedFrom)` (or Denoise)
  to Effects, instead of one per call of the chain. It still reads 0 to 1,
  monotone; there are fewer end-of-call reports.

**Users.** `PreviewRenderer` only, with four ladders, as today's four caches:
CPU shown, CPU background, GPU shown, GPU background. `CheckpointCache` is
deleted. Exports, the CLI, thumbnails and Python render once and take no
ladder; they go through the same driver with none. Later: roadmap (b)'s render
facade owns the ladders it renders through; step 2's mask edits need nothing,
since they change the pointwise block and resume from the Denoise rung (or from the source with noise reduction off).

### 9. What moves where

| From | To | Sub-step |
|---|---|---|
| `ProcessingPlan`'s 13 flat pointwise fields | `PointwisePlan` (`toWorking`, `shoulderKnee`, `presence`, `toneCurves`, `colorAdjustments`) and `TonePlan` (the eight tone fields) | A |
| `ProcessingPlan.h`: perceptual helpers, weights, `smoothstep`, `liftedBlackThreshold`, `shapeLuminance`, `shapeTone`, `rollHighlights`, `tonePlanFor` | `TonePlan.h` | A |
| `ProcessingPlan.h`: `curveRatioFloor`, `applyToneCurves`, `developToCurveInput`, `developPixel`, `developToTap`, `curveInputFieldsOf` | `PointwisePlan.h` | A |
| `ToneSettings.cpp` (`contrastSlopeFor`, `toneShiftFor`, `shoulderKneeFor`, reaches, `tonePlanFor`) | `TonePlan.cpp`: `toneAmountsOf`, `tonePlanFor`, `shoulderKneeFor`; inline resolution functions and reaches in `TonePlan.h` | A |
| `PresencePlan::texture`, `clarity`, `dehaze` | `PresencePlan::amounts` (`PresenceAmounts`) | A |
| `ColorAdjustmentPlan::adjustsSaturation`, `saturation`, `adjustsVibrance`, `vibrance` | `ColorAdjustmentPlan::chroma` (`ChromaAmounts`) | A |
| `packPointwise(const ProcessingPlan&, probe)` | `packPointwise(const PointwisePlan&, ImageSize source, probe)` | A |
| `develop.frag` functions reading `plan.*` tone, Presence amounts, saturation and vibrance | functions taking `TonePlan`, `PresenceAmounts`, `ChromaAmounts`; `main` builds `globalAmounts()` | A |
| CPU `runStages`, `developFromSource`, `pointwiseFromSource`, `denoisedCopy`, `developPointwise` | `CpuStages` + `HostPixels` + `runStages<CpuStages>` | B |
| GPU `runPasses`, `developPasses` | `GpuStages` + `runStages<GpuStages>`; `pointwiseOnGpu`, `geometryOnGpu`, `effectsOnGpu` extracted | B |
| Skip conditions in both drivers; `stepAfter`/`stepThrough` switches; `staleReason`'s size switch | `stageTable` (`runs`, `firstStep`/`lastStep`, `sizeAt`) | B |
| `app::CheckpointCache` (PreviewRenderer.cpp) | `CheckpointLadder`, `LadderAccess`, `resumeOrDevelop`, `resumeOrDevelopOnGpu` | B |

## Sub-step A: `PointwisePlan` and `TonePlan` extraction

Status: done, 2026-10-09 (uncommitted); digests identical to the baseline on both backends.

Plan, chain and GPU packing. No public header changes, no uniform-layout
change, no behaviour change.

A0. **Digest harness first, before any other change.** Add
`tests/test_RenderDigest.cpp` (in `arraw-tests`) and
`tests/gpu/test_GpuRenderDigest.cpp` (in `arraw-gpu-tests`), one hidden
Catch2 case each, tagged `[.digest]`, which do nothing unless
`ARRAW_DIGEST_OUT` names a file. Each renders a fixed matrix and appends one
line per render, `<case> <width>x<height> <format> <sha256 of the bytes>`
(`QCryptographicHash::Sha256`):
- sources: `linear-32x24-warmwb.dng`, `bayer-32x24.dng`,
  `testcard-61x41-srgb8.png`, `testcard-61x41-alpha8.png`, and
  `test::rainbow({97, 61}, PixelFormat::RgbaF32, workingEncoding)`;
- states: default; Exposure, Contrast and the four tone controls; plus
  Filmic Highlights; a luma curve lifting black plus a red curve; HSL; Black &
  White; Colour Grading; Saturation and Vibrance; Texture; Clarity; Dehaze +60;
  Dehaze -60; luminance and colour noise reduction; straighten with a crop; an
  orientation with a flip; vignette and grain with a fixed seed; everything at
  once;
- requests: default; `FitInside{40, 40}` Lanczos3; `Scale{0.5}` Bilinear; a
  region `{0.2, 0.1, 0.7, 0.9}` with `FitInside{20, 20}`;
- paths: CPU `develop`, `developUntil` at each of the five stops (read back),
  `sample` at `Tap::CurveInput`; GPU `developOnGpu` at each stop (read back),
  `sampleOnGpu`.
Use only API that exists at the baseline commit, so the same two files build
there. Capture the baseline: `git worktree add` at the commit before A, copy
the two files and their CMake lines in, build, run
`ARRAW_DIGEST_OUT=… arraw-tests "[.digest]"` and the GPU binary likewise.
The files stay committed with A, as a tool for later refactors.

A1. **`src/core/TonePlan.h` and `TonePlan.cpp`** (`git mv` of
`ToneSettings.cpp`; update `CMakeLists.txt`). Move into the header, verbatim
apart from parameter types: `toPerceptual`, `toLinear`, `greyPivot`,
`smoothstep`, the four weights, `liftedBlackThreshold`, `shapeLuminance`,
`shapeTone` (now `const TonePlan&`), `rollHighlights` (now
`float shoulderKnee`). Add `ToneAmounts`, `TonePlan` (eight fields, the
current order, defaulted `==`), `regionalReach` and `endpointReach` (moved
from the `.cpp`), the inline resolution functions and `resolveTone` of §2,
each body today's expression unchanged. In the `.cpp`: `toneAmountsOf`
(today's finiteness checks, messages and order, then the clamps),
`tonePlanFor` returning `TonePlan`, `shoulderKneeFor` unchanged. Doxygen
briefs per CLAUDE.md; keep each "Mirrored by develop.frag" note.

A2. **`PresenceAmounts`** in `Presence.h`: `struct PresenceAmounts { float
texture, clarity, dehaze; }` with defaulted `==`, member `amounts` of
`PresencePlan` replacing the three floats, `presenceAmountFor`. In
`Presence.cpp`, `presencePlanFor` uses it (the base decisions read
`plan.amounts.*`), and `applyPresence` gains the `const PresenceAmounts&`
parameter; the four-argument form passes `plan.amounts`.
`presenceContextFieldsOf` unchanged.

A3. **`ChromaAmounts`** in `ColorAdjustments.h`: the four fields move into
`struct ChromaAmounts`, member `chroma` (first, where they were), with
`chromaAmountsFor(float saturation, float vibrance)` used by
`colorAdjustmentPlanFor` after its clamps. `adjustColor(plan, chroma, colour)`
plus `adjustColor(plan, colour)` passing `plan.chroma`.

A4. **`src/core/PointwisePlan.h`**: `PointwisePlan` (members of §1 in that
order, defaulted `==`), `PixelAmounts`, `globalAmountsOf`; move
`curveRatioFloor` and `applyToneCurves` (now `const ToneCurvePlan&`),
`developToCurveInput`, `developPixel` (the three overloads of §3),
`developToTap` and `curveInputFieldsOf` (now
`tie(toWorking, tone, presence)`) from `ProcessingPlan.h`. Keep the doc
comments, adjusted to the new parameters.

A5. **`ProcessingPlan`**: members `denoise`, `pointwise`, `geometry`,
`resize`, `effects`; `stagesOf` the flat tie of §4; `sameAtTap` through
`curveInputFieldsOf(first.pointwise)`; `ProcessingPlan.h` includes
`PointwisePlan.h`; update the struct's and `stagesOf`'s comments (the plan is
no longer "partly flat"). `planFor(encoding, …)` fills `plan.pointwise` in
today's order (`tonePlanFor`, then `shoulderKneeFor`, `colorMatrixFor`,
denoise, curves, colour, effects), so the same invalid input throws the same
message first. The buffer and `Photo` overloads write `plan.pointwise.presence`.

A6. **Engine call sites**: `Develop.cpp` (`runPointwise` and the chains take
`const PointwisePlan&`; the traversal computes `globalAmountsOf` once per
render and calls the four-argument `developPixel`), `RenderProgress.cpp`
(`plan.pointwise.presence`), any other `src/core` reader the compiler finds.
`GpuPlan.{h,cpp}`: `packPointwise(const PointwisePlan&, ImageSize source,
PointwiseProbe)` reading `tone`, `shoulderKnee`, `colorAdjustments.chroma`,
`presence.amounts`, and the grid sizes from `source`. `GpuDevelop.cpp`:
`packPointwise(plan.pointwise, image.size(), probe)`,
`packToneCurves(plan.pointwise.toneCurves)`, `presenceOnGpu(…,
plan.pointwise.presence)`; remove the `assert` on `geometry->sourceSize`.

A7. **`develop.frag`**: add the four structs of §3 and `globalAmounts()`;
`shapeLuminance`, `shapeTone`, `applyPresence` and `adjustColor` take them in
place of reading `plan.exposureGain` … `plan.whiteShift`,
`plan.shapesTone`, `plan.textureAmount` … `plan.dehazeAmount`,
`plan.adjustsSaturation` … `plan.vibrance`; `main` builds
`const PixelAmounts amounts = globalAmounts()` once and keeps its stage order,
probes and the component-wise exposure multiply exactly as written. The
`Pointwise` block, bindings and the header comment's contract are unchanged;
add that the structs mirror `TonePlan.h`, `Presence.h`,
`ColorAdjustments.h` and `PointwisePlan.h`.

A8. **Tests.** Update field paths and call sites mechanically
(`plan.exposureGain` → `plan.pointwise.tone.exposureGain`,
`shapeTone(plan, …)` → `shapeTone(plan.pointwise.tone, …)`,
`packPointwise(tonePlanFor(…))` → `packPointwise(PointwisePlan{.tone =
tonePlanFor(…)}, {})`, `tonePlanFor(…).shoulderKnee` → `shoulderKneeFor(…)`,
and so on). No expected value, tolerance or tag changes. New:
- `STATIC_REQUIRE(test::fieldCount<ProcessingPlan> == stageCount)`,
  `fieldCount<PointwisePlan> == 6`, `fieldCount<TonePlan> == 8`;
- `test_RenderCheckpoint`'s prefix test varies each `TonePlan` field and
  `shoulderKnee` through `plan.pointwise` and still asserts full-depth
  `prefixMatches` equals `==`;
- `resolveTone(toneAmountsOf(s))` equals `tonePlanFor(s)` bit for bit, and
  each resolution function equals its field, over settings at, inside and
  beyond the limits;
- `toneAmountsOf` throws for each non-finite field, with today's message;
- `packPointwise` takes its grid sizes from the size it is given.

A9. **Docs**: a dated note in ADR 011 (`stagesOf` now ties one block per
stage, the shape it sketched; `fieldCount` is the guard), in ADR 044 (the
local block is `PointwisePlan::local`; the chain lives in
`PointwisePlan.h`, `smoothstep` in `TonePlan.h`), and in ADR 041 (the
amounts are `PresencePlan::amounts`). Mark A done in this note.

A10. **Verify**: `just format`, `just format-check`, `just build`, `just test`
(GPU tests run on lavapipe), `just py-test`, the digest diff against the
baseline (empty), and a timing of the CPU pointwise boundary as in
ADR 041 (release, best of five, a large source; equal within noise). Do not
commit; the user decides.

## Sub-step B: stage table, driver, `CheckpointLadder`

Status: done, 2026-10-09 (uncommitted); digests identical to the baseline on both backends. Step 1 of
`local-adjustment-plan.md` is done.

B0. **Baseline**: the digest from A (or from before A; they are equal).

B1. **`src/core/StageTable.h`**: `StageRow`, `stageTable`, `rowOf`,
`following`, `keepsRung` of §6 and §8, with a `static_assert` that row *i* is
`Stage` *i*. The `runs` predicates are exactly §6's column.

B2. **Table reads**: `detail::stepAfter` and `detail::stepThrough`
(`RenderProgress.cpp`) from `firstStep`/`lastStep`; `staleReason`'s expected
size from `sizeAt`. Behaviour identical; the existing progress and checkpoint
tests cover it.

B3. **`src/core/StageDriver.h`**: the `StageBackend` concept, `StagesRun`,
`runStages` of §7.

B4. **CPU backend** (`Develop.cpp`): `HostPixels`, `CpuStages`; re-express
`develop`, `developUntil`, `resumeFrom`, `canResumeFrom` (unchanged logic),
`sample` per §7's table; delete `runStages` (the old one),
`developFromSource`, `pointwiseFromSource`, `denoisedCopy`,
`developPointwise`. Keep every `TimingSpan` name and `ProgressRoot` start and
finish step. `applyGeometry`, `resample` and `effectsBy` keep their own
identity checks (harmless; the driver no longer reaches them for an identity).

B5. **GPU backend** (`GpuDevelop.cpp`): `GpuStages`; extract
`pointwiseOnGpu` (from `runPasses`' Pointwise branch, unchanged),
`geometryOnGpu`, `effectsOnGpu`; re-express both `developOnGpu` overloads, the
resume overload and `sampleOnGpu`; delete `runPasses`, `PassResult`,
`developPasses`. The render count of every path stays as today
(`GpuContext::renderCount`).

B6. **`CheckpointLadder`**: `include/CheckpointLadder.h` and
`src/core/CheckpointLadder.cpp` (public part), `src/core/LadderAccess.h`
(`bind`, `deepestUsable`, `store`); `resumeOrDevelop` in `include/Develop.h`
and `Develop.cpp`; `resumeOrDevelopOnGpu` in `src/gpu/GpuDevelop.{h,cpp}`.
Rules exactly as §8. Add the new files to `CMakeLists.txt`. Doxygen per
CLAUDE.md.

B7. **`PreviewRenderer`**: delete `CheckpointCache`; `run()` holds
`CheckpointLadder cpuLadder` and `backgroundLadder`, `GpuPreview` holds
`ladder_` and `backgroundLadder_`; `render()` calls `resumeOrDevelop` /
`resumeOrDevelopOnGpu` and copies `resumedFrom` into the result;
`bind`/`clear` calls become `clear()` where the source, the device or a
failure changes (binding to the level is now the engine's). Update the class
comment (the engine keeps the rungs; the renderer clears them when the device
changes).

B8. **Tests** (`tests/test_CheckpointLadder.cpp`,
`tests/gpu/test_GpuLadder.cpp`):
- a ladder render equals `develop` (CPU) / `developOnGpu` (GPU) bit for bit,
  fresh and resumed from each rung, over the digest's states;
- after a fresh render the ladder holds Pointwise, Geometry and Resize, and
  Denoise only with noise reduction on; never Effects;
- an edit at each stage resumes from the expected rung and drops the deeper
  ones (the sequence of `test_PreviewRenderer`'s "An edit resumes from the
  newest checkpoint it can still use", at engine level);
- another source buffer clears the ladder; `clear()` empties it;
- the CPU entry drops resident rungs and the GPU entry drops host rungs or
  another device's, then renders from the source;
- a bad request throws and leaves the ladder as it was;
- a cancellation part-way keeps the rungs finished before it, and the next
  render resumes from them;
- an observed ladder render reports monotone progress ending at 1, from the
  resumed rung's share;
- on the GPU, a fresh ladder render issues as many renders as a fresh
  `developOnGpu` to Effects, and a resume from Resize with effects off issues
  none.
Existing suites unchanged, `test_PreviewRenderer` included.

B9. **Docs**: an ADR 045, "A render runs one stage table and keeps its
checkpoints on a ladder" (the table, the driver, the ladder's rules and
users, what was rejected: §"Alternatives"), with dated notes in ADR 011
(`stopAfter`/`resumeFrom` and the processor that caches: the ladder), ADR 015
(device ladders belong to the device's thread) and ADR 024 (the preview's
cache moved into the engine). Mark B and step 1 done in this note and in
`local-adjustment-plan.md`.

B10. **Verify**: as A10, the digest diff empty, plus a manual run of the GUI:
a tone drag, a straighten, a zoom and pan, and a vignette drag each update,
on the GPU and with the CPU forced. Do not commit; the user decides.

## Verification: what proves "no output change"

- **The digest (A0)** is the bit-identical proof: every public render path,
  on both backends, over the matrix, before and after each sub-step, compared
  as SHA-256 of the read-back bytes. It runs on one machine and compiler
  (x86-64 Linux, lavapipe), where float maths is not contracted (no FMA in
  the baseline ISA). If the GPU digest differs after A7 while the CPU one
  does not, the shader compiler treated the restructured functions
  differently: stop and report; whether a difference within the parity
  tolerance is acceptable is the user's decision, not the implementer's.
- **Existing tests**, unchanged in what they assert: the CPU/GPU parity
  suites (`tests/gpu/test_GpuPointwise.cpp`, `test_GpuPresence`,
  `test_GpuDevelop`, `test_GpuSample`, …), the resume-equals-fresh tests
  (`test_RenderCheckpoint`, `test_GpuDevelop`), the progress tests
  (`test_Progress`, `test_GpuProgress`, which already assert that an observed
  render has the bits of an unobserved one), the span unit-count tests, the
  shader-layout reflection test, `test_PreviewRenderer` (resume sequence, bit
  for bit against a fresh renderer), `test_ExportQueue`, the CLI tests and
  the Python suite with its CLI parity test.
- **Review rule for test diffs**: in A, test changes are field paths and call
  signatures only; in B, existing tests change not at all. Anything else in a
  test diff is a finding.

## Deliberately not done

- **`PreviewPipeline` and `CropModeController` out of `MainWindow`.** Neither
  exists yet, and nothing in steps 1 and 2 needs them. They belong before
  step 3 (the masks GUI), whose mask tool is "a controller like
  `CropModeController`".
- **Anything of ADR 044 itself**: `LocalPlan`, per-pixel amounts, the GLSL
  resolution functions, the Temp/Tint gain, the two haze bases and base flags
  in the uniform block.
- **No device seam or render facade** (roadmap (b)). The ladder is designed
  to be owned by that facade later; core still does not link the GPU.
- **No borrowed-input stages.** CPU resumes still clone once per rung resumed
  from, as ADR 024 records; sharing pixels between rungs at a collapsed
  boundary (a shared payload in `CheckpointState`) is not attempted either.
- **No Effects rung**, no short-circuit for an identical repeated request
  (it resumes from Resize, as today).
- **No source identity in the plan** (ADR 012's decode block): the ladder
  binds to the buffer's address, as the cache did.
- **No region-restricted earlier stages** (ADR 025's future work).
- **No caching of the Presence context** (ADR 041, ADR 044 §5: optional).
- **No change** to `Stage`, `RenderCheckpoint`, `CheckpointState`, the uniform
  blocks, the bindings, error messages, the Python bindings or the CLI.
- **The front ends' private includes** (`CurveHistogramRefresh.h` holds a
  `ProcessingPlan`) stay as they are.

## Alternatives considered

- **A virtual backend interface over `CheckpointPixels`** instead of the
  driver template. It would avoid a template, but every stage would unwrap a
  variant, and the CPU's borrowed source (any layout, not a checkpoint) has no
  place in that variant. One template with a three-function concept is
  smaller.
- **One table holding function pointers for both backends.** Impossible
  without core naming GPU functions (it does not link `arraw-gpu`), unless
  through the device seam of roadmap (b). The shared rows plus one `switch`
  per backend give the same single place for the skip rule.
- **Deriving `stagesOf` from member pointers in the table.** Heterogeneous
  block types need a tuple and more template code for no gain once the plan is
  one block per stage; `stagesOf` is five names.
- **`TonePlan` with the shoulder.** It would keep `tonePlanFor(…).shoulderKnee`
  in tests, but the per-pixel unit would carry a global value, and
  `curveInputFieldsOf` could no longer tie the whole block.
- **Presence and chroma amounts left in place until step 2.** Smaller A, but
  step 2 would then reshape `PresencePlan`, `ColorAdjustmentPlan`, the packing
  and both chains while also adding masks; A does the reshaping with no
  output change to hide behind.
- **A private ladder.** The app may include `src/core`, so it would compile;
  but the ladder-taking verbs belong beside `develop` and `developOnGpu`, and
  a public type keeps Python and the services layer open.
- **Notes in ADR 024 instead of ADR 045.** Possible; a new public type and
  verb have had their own ADR so far.

## Open questions

1. A2 and A3 (Presence and chroma amounts) could move to step 2 if A should be
   smaller. Recommended: keep them in A.
2. `CheckpointLadder` public (recommended) or private.
3. ADR 045 (recommended) or dated notes only.
4. "Per-pixel resolution functions shared by both" is read here as: one C++
   definition shared by the planner and, in step 2, the CPU chain; the GLSL
   mirrors follow in step 2, since writing them now cannot be tested without
   changing GPU output.
5. The digest test files committed (recommended, hidden by tag) or kept as a
   scratch tool.
