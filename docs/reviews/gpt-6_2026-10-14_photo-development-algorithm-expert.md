# Algorithm-expert review of the photo-development reference

Date: 2026-10-14. Model: GPT-6. Perspective: image-processing algorithm developer.

Scope: a second review pass over `docs/photo-development.md`, checking the equations, photographic interpretation and current implementation. I inspected the tone, curve, colour, grading, RAW settings, denoise and Presence code directly, and read the remaining reference for consistency. This is not an independent implementation, numerical conformance suite or empirical image-quality evaluation. I contributed the original tone/colour text, so this is a fresh review perspective rather than independent authorship. No target documentation or production code was changed during this review.

## Findings

### Medium — The grading tint equation needs an explicit degree-to-radian conversion

Location: section 5, “Three-zone colour grading and split toning”, currently lines 376–380.

The prose defines `theta` in degrees, but the tint equation immediately uses `cos(theta), sin(theta)`. Read literally with mathematical trigonometric functions, or implemented in C++/Python from the reference, this produces the wrong tint. For example the documented 30-degree red-ish direction would be evaluated as 30 radians. The implementation explicitly computes `angle = hue * pi / 180` in `tintOf` in [ColorGrading.cpp](../../src/core/ColorGrading.cpp).

Recommended edit: write `phi_i = pi theta_i / 180` and use `(cos(phi_i), sin(phi_i))`, or add degree symbols inside the trigonometric arguments. This is a documentation defect, not an implementation defect.

### Medium — Channel-curve negative/zero handling reuses the luma lift symbol without redefining it

Location: section 4, “Tone curves, including individual colour channels”, currently lines 274–280.

The luma-curve paragraph defines `ell` from the luma curve's black endpoint. The next paragraph then says a negative channel maps to `c + ell` and zero/NaN becomes `ell`, while explaining the red, green and blue curves. Each channel actually uses **its own curve's** lift, independently of the luma curve. A reader implementing the equations literally would get wrong black and out-of-gamut behaviour whenever channel endpoint values differ, or the luma curve is inactive.

Evidence: `applyToneCurves` in [PointwisePlan.h](../../src/core/PointwisePlan.h) computes the luma lift separately, then the channel lambda computes `toLinear(max(evaluateCurve(curve, 0), 0))` from the curve passed to that lambda.

Recommended edit: define `F_k` and `ell_k = max(F_k(0),0)^2.2` for channel `k`; use these symbols throughout the channel paragraph. This is a documentation ambiguity with a concrete wrong numerical interpretation, not an implementation defect.

### Low — The RAW table's “initial integer result has ... headroom” is insufficiently qualified

Location: section 3.1, Transfer and precision row, currently line 107.

The decoded samples are unsigned 16-bit values subsequently normalized by 65535, and LibRaw is explicitly configured with `highlight=0`. That buffer cannot retain distinct code values above its representable maximum. “Has headroom” can be read as a promise that uncapped scene highlights survive initial decoding, undermining the accurate clipping warning elsewhere in the same table. Floating-point development headroom above reference white is a different property; it can be created by the camera matrix or subsequent exposure.

Recommended edit: say “Linear 16-bit samples; highlights that reach the decode maximum are clipped. Floating-point stages can subsequently carry values above working reference white.” If reserving “headroom” for a specific calibrated scene-white relationship, define that relationship first. This is a wording issue, not evidence that the import implementation is broken.

## Photographic model assessment

The reference faithfully describes pragmatic artistic operations rather than claiming physical reconstruction. Exposure is a conventional linear gain; the pivoted power contrast, sequential smooth windows, bounded detail gains and shoulder have understandable behaviour. The distinction between the Highlights window and the filmic shoulder is particularly valuable. The source code's signed/out-of-gamut treatment and near-black curve-ratio floor are documented unusually carefully.

Several limitations should remain explicit, as they are in the document:

* “HSL” uses HSV in linear Rec.2020. Its value control is additive, can strongly affect near-black colours, and is not a perceptually uniform brightness adjustment. This is an implementation design choice, accurately disclosed; renaming or redesigning the algorithm would be a separate product decision.
* Normalizing overlapping hue weights dilutes an isolated band's slider even at that band's centre. The red-band example usefully exposes this surprising behaviour rather than suggesting Lightroom equivalence.
* Oklab chroma scaling and grading hold Oklab lightness, not linear luminance. They can produce out-of-gamut colours; the absence of a final perceptual gamut mapper is an implementation limitation, not a documentation equation error.
* The shoulder loses chroma intentionally and cannot recover sensor clipping. No amount of precise mathematics should turn this into a RAW highlight-reconstruction claim.
* Separable bilateral denoise and the Dehaze morphology are practical approximations, not calibrated sensor-noise or atmospheric-scattering models. The reference appropriately acknowledges their limits.

One additional implementation characteristic may merit a short optional note: Filmic Highlights is disabled exactly at zero, whereas the positive-setting limit compresses above-white values toward white. Thus its setting response is not continuous at zero for inputs above white. The present equations describe this correctly; this is not a defect in the document and does not require an algorithm change for this documentation task.

## Strengths and conclusion

The pipeline order agrees with `PointwisePlan.h`: matrix/balance, exposure, Basic Tone, Presence, curves, shoulder, colour/B&W and grading. Texture and Clarity correctly share a summed stop gain. The tone-window table, contrast exponent mapping, LUT approximation description, B/W saturation-weighted gain and grading zone weights agree with the inspected code. The text distinguishes the tone power coordinate, linear luminance, HSV value and Oklab lightness, preventing a common source of misleading photographic explanations.

No high-severity mathematical or pipeline-order defect was found in the inspected material. Correct the two notation issues before treating the equations as directly reproducible reference material; qualify initial RAW headroom wording. No production changes are recommended as part of this review.
