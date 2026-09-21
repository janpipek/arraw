# White-balance review and explanation

Reviewed commit `23c92c5cf955c67f785bee6e98b17d83aec74f64` — “Name the light, in kelvin, per camera”, against parent `2a73a9a1e037b5eb35263b48c1996ea4a486a900`.

Scope: the library implementation, its tests, and its agreement with ADRs 007 and 008. Production code was not modified. Standards and specification were reviewed independently by parallel agents; the findings below include additional local numerical and integration checks.

The overall design is appropriate. Keep the camera-space correction and the calibration data. Two correctness gaps need attention: resolving missing settings when the RAW has no recorded neutral, and validating settings before they reach pixel arithmetic. The implementation can also be shortened without replacing its colour model.

## What the commit does

The preceding commits made RAW decoding return pixels in the camera's channels. Those pixels already have the decode's white-balance gains applied. This commit adds a library-level choice between `AsShot` and `Custom`, with optional temperature and tint; it does not add GUI sliders or CLI flags.

There are two translations:

1. **Temperature/tint → gains.** Temperature chooses a point along the curve of blackbody light colours. Tint moves away from that curve along a same-temperature line. The code represents this position in CIE 1960 `u,v`, converts it to XYZ, then uses the camera calibration to predict how that light would register in the sensor's three channels. Taking the reciprocal of those channel responses gives gains that neutralise the light. The gains are rescaled so green is one.
2. **Recorded gains → temperature/tint.** Take the reciprocal of the recorded gains to recover a neutral's camera-channel ratios, transform those to XYZ, discard brightness to obtain `u,v`, and find the corresponding position relative to the same table. This supplies the values to display when opening a photograph.

The table and tint scale follow the Robertson/DNG SDK approach. I checked the structure of both conversions against [Adobe's DNG SDK temperature implementation, mirrored in AOSP](https://android.googlesource.com/platform/external/dng_sdk/+/refs/heads/android14-prebuilt-test/source/dng_temperature.cpp). This supports the algorithm choice, not a claim of identical Lightroom rendering; ADR 007 explicitly defers full camera profiles and dual-illuminant calibration.

The camera calibration has two necessary pieces:

- `toWorking`: the normalised camera-to-Rec.2020 matrix.
- `daylightScale`: the channel scales LibRaw removed when constructing that matrix.

The matrix alone cannot distinguish two sensors with different channel sensitivities. Restoring the scales is essential for meaningful Kelvin values.

Development then computes:

```text
delta[channel] = wantedGain[channel] / alreadyAppliedGain[channel]
output RGB     = 2^exposure × toWorking × diagonal(delta) × decoded RGB
```

For example, if decoding already applied gains `(2, 1, 1.25)` and the requested setting wants `(3, 1, 1)`, the additional gains are `(1.5, 1, 0.8)`. Applying the wanted gains directly would white-balance the photograph twice.

`AsShot` bypasses conversion and uses exactly `(1,1,1)` as the delta. Default development therefore retains the decoded balance, without round-trip numerical error. Custom WB is folded into the colour matrix once per image; it does not add another image pass. Exposure scales RGB and alpha passes through unchanged. Custom absolute temperature on an already-developed image is explicitly refused; incremental non-RAW controls remain deferred.

## Standards

No substantial standards violations were found. The public API stays in `include/` and namespace `arraw`; the implementation uses ordinary C++20 values and standard-library facilities without unnecessary class hierarchies.

- **Minor documented violation:** [`WhiteBalance.cpp:107`](../../src/core/WhiteBalance.cpp#L107) gives `isothermDirection()` the noun-form brief “Direction of the same-temperature line…”. `AGENTS.md` requires verb-form function briefs. “Computes the unit direction of the same-temperature line” would comply.
- **Documentation judgment call:** Several new implementation comments use `//`, while `AGENTS.md` says “Comments in Doxygen style, /// rather than /*.” Whether this extends to ordinary implementation comments is ambiguous; it is cleanup, not a correctness issue.

`just format-check` passes.

## Spec

### F1 — P2: Missing-neutral files resolve Custom defaults from the wrong gains

Location: [`Develop.cpp:46`](../../src/core/Develop.cpp#L46), with the recorded-gain conversion in [`WhiteBalance.cpp:256`](../../src/core/WhiteBalance.cpp#L256).

ADR 007 says: “Anything computing a temperature from the buffer needs the applied ones.” For a RAW without an as-shot neutral, `RawImport.cpp` stores placeholder recorded gains `(1,1,1)` but applies the camera's daylight gains. The new code nevertheless resolves absent temperature/tint from the placeholder recorded gains.

Consequences: the reported temperature does not describe the initial rendering, switching to `Custom` with both values absent changes colour, and a tint-only edit can also change temperature.

**Reproduction:** Using the existing fixture generator, create a temporary DNG with `SKEWED_COLOR_MATRIX_1`, no `AsShotNeutral`, and constant RGB samples `(10000,10000,10000)`. Load it through the normal public import API:

| Measurement | Result |
|---|---|
| Recorded placeholder gains | `(1,1,1)` |
| Applied gains | `(0.454572,1,1.25024)` |
| `asShotTemperature(camera)` | `12342.1 K`, tint `70.5867` |
| Temperature inferred from applied gains | `6502.09 K`, tint `9.76663` |
| Maximum sample change: `AsShot` → `Custom{}` | `0.122706` |

**Recommendation:** Resolve unspecified controls from the effective applied balance. Preserve the distinction between recorded and substituted WB for reporting; an optional recorded neutral or an explicit provenance flag would make that distinction reliable. Do not infer absence merely from unity gains, which can be legitimate. Add an integration case combining non-unity calibration with a missing neutral; the existing missing-neutral fixtures have nearly unity calibration and conceal this error.

### F2 — P2: Invalid settings reach pixel arithmetic

Location: [`WhiteBalance.cpp:130`](../../src/core/WhiteBalance.cpp#L130) and the Custom path in [`Develop.cpp:46`](../../src/core/Develop.cpp#L46).

ADR 008 says: “The processing contract clamps as well, redundantly, so that no pixel maths depends on the reader having done it.” The new conversion rejects only temperatures `<= 0`; tint has no input validation, and neither value is checked for finiteness.

**Reproduction through the public API:**

- NaN temperature or NaN tint returns gains `(NaN,1,NaN)` and causes `develop()` to return NaN in all RGB channels.
- Positive infinite temperature is accepted and produces finite but arbitrary-looking output.
- `1500 K` is extrapolated beyond the table and converts back to approximately `1666.67 K`.
- `30000 K` passes through without renderer clamping, contrary to the bounded processing policy described in ADR 008.

**Recommendation:** Define explicit model limits for temperature and tint, reject nonfinite values, and implement the specified renderer boundary policy after resolving the WB mode. Keep default AsShot independent of clamping a reconstructed temperature. The ADR uses 2000–12000 K as examples, but the actual supported limits should become named model data rather than being inferred from test inputs.

The commit otherwise implements its stated slice. Descriptors, non-RAW incremental controls, CLI/UI controls, sidecars and preset workflows are broader deferred ADR work. The missing-neutral warning channel also remains outstanding; F1 is already observable without any of those features.

## Simplification options

### 1. Recommended: Build the camera-to-XYZ transform directly

Currently `xyzToCamera()` reconstructs camera-to-sRGB, inverts it, divides rows by the daylight scales, and composes XYZ-to-sRGB. `asShotTemperature()` then inverts that entire result again.

Let `M = camera.toWorking`, `D = diagonal(camera.daylightScale)`, and `W = XYZ-to-working`. The current transform is algebraically:

```text
XYZ-to-camera = D^-1 × M^-1 × W
camera-to-XYZ = W^-1 × M × D
```

This suggests one private helper:

```cpp
constexpr Matrix3 xyzToWorking = colorspaces::srgbToWorking * colorspaces::xyzToSrgb;
constexpr Matrix3 workingToXyz = xyzToWorking.inverse();

Matrix3 cameraToXyz(const CameraNative& camera) {
    // Retain validation of the calibration scales here.
    return workingToXyz * camera.toWorking * Matrix3::scale(camera.daylightScale);
}
```

Use it directly when reading gains back as temperature, and invert it when computing gains. This removes the runtime row-scaling loop and both runtime inversions from the reverse conversion. The forward conversion still needs one inversion. It also expresses the operation in the direction naturally supplied by `CameraNative`.

This preserves the mathematical mapping, not bit-identical floating-point evaluation. On the committed skewed fixture, a temporary probe found maximum matrix-coefficient differences of about `3.0e-8` in the direct direction and `9.5e-7` in the inverse direction. Keep numerical regression checks when implementing it; these measurements are evidence for the algebra, not exhaustive validation for every camera.

### 2. Smaller alternative: Simplify only settings resolution

If leaving the calibration calculation untouched for now:

- Recover a fallback temperature only when temperature or tint is absent. Fully specified Custom settings currently calculate the as-shot value unnecessarily.
- After normalising wanted and applied gains, return `{wanted[0]/applied[0], 1, wanted[2]/applied[2]}`. The final `withGreenAtOne()` is redundant because both green components are exactly one.
- Consider a private `temperatureForGains(camera, gains)` helper. It gives effective and recorded WB an explicit common conversion and avoids modifying a copy of `CameraNative` just to inspect another set of gains.

The first two changes preserve ordinary valid-input behaviour; choosing the correct fallback is the separate F1 fix. Neither option requires new public classes or a cache.

### What should remain

Keep the Robertson table, separate AsShot mode, optional setting components, camera-space application, and daylight calibration. Replacing them with a short temperature-to-RGB approximation would lose the functionality this commit is intended to establish. Much of the 263-line conversion file is explanation and fixed reference data, so reducing line count alone is not a useful target.

## Validation and test coverage

- `just test`: **84/84 passed**.
- `just format-check`: passed.
- Temporary C++ probes linked against the built library reproduced F1 and F2. The extra DNG and probe sources/binaries were written under `/tmp`.
- An additional 17,017-point synthetic-camera sweep covered 2000–12000 K in 10 K steps and tint -40–40 in steps of 5. Among accepted points, the worst errors were approximately **0.0762 K** and **0.000298 tint**. The converter rejected 51 combinations because that synthetic camera's predicted channel response was nonpositive. Thus the round trip is numerically very good where representable, but “exact” should not mean a mathematical or bitwise identity.
- The illuminant references and skewed fixture are useful additions. A remaining test improvement is an independent numerical expectation for a developed skewed-camera pixel. Current assertions mainly check qualitative direction, round-trip consistency and matrix properties; `toWorking != identity` alone does not establish that the skew was reconstructed correctly, since even sRGB-to-Rec.2020 is nonidentity.
- No Windows/macOS build or real-camera RAW corpus was exercised. Highlight clipping and full Adobe profile matching are existing, explicitly documented limitations, not new findings against this commit.

Standards: 1 minor definite finding plus 1 documentation judgment call; worst is the function-brief convention. Spec: 2 P2 findings; missing-neutral fallback and invalid-input handling both affect output correctness.
