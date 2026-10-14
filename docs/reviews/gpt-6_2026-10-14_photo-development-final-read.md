# Photo-development reference: final explanatory review

Date: 2026-10-14. Model: GPT-6.

Reviewed [the development reference](../photo-development.md) end to end, with particular attention to chapters 6–9 following their LaTeX conversion. This is a review of the pre-expansion document; production code was not modified.

## Findings to address

1. **Medium: Dehaze colour input is ambiguous.** Chapter 6 defines the combined Texture/Clarity gain and subsequently reuses the symbol for the original colour in the positive and negative Dehaze formulas. Define a new intermediate colour, such as $\mathbf c_t=2^s\mathbf c$, and use it in both Dehaze branches. Their luminance $T$ already refers to this gained colour. This prevents accidentally applying the gain to the luminance but omitting it from RGB. Verified against [Presence.cpp](../../src/core/Presence.cpp).

2. **Medium: Dehaze saturation amount uses a different unit convention.** “Saturation operator with amount $0.16ah$” is a normalised internal amount, whereas chapter 5 defines saturation using slider values from −100 to +100. State the resulting Oklab chroma multiplier explicitly: $1+0.16a_{\mathrm{dehaze}}h$. At full positive strength and full veil share this is a 16% chroma increase, equivalent to +16 saturation slider units. Negative Dehaze has the corresponding negative multiplier change. Verified against [Presence.cpp](../../src/core/Presence.cpp) and [Presence.h](../../src/core/Presence.h).

3. **Low: the black-luminance bypass applies to all Presence controls.** The chapter currently mentions only Texture/Clarity when describing the lifted-black threshold. The early return precedes Dehaze too. Give its actual value, $10^{-20}$, and mention nonfinite handling consistent with the comparison. Verified against [Presence.cpp](../../src/core/Presence.cpp) and [TonePlan.h](../../src/core/TonePlan.h).

## Useful additions

- Explain that the denoise colour ratio has unit luminance under the source luminance row: $\mathbf r\cdot\mathbf q=1$. It is a vector carrying colour proportions, rather than an HSV saturation or ordinary channel division by brightness at black. A neutral example makes the separation understandable without following its derivation.
- Add one numerical Dehaze example showing how a floor one stop below a coarse cell yields half the floor share before white protection. This connects the logarithmic context construction to the visible attenuation.
- Spell out the mask coordinate conversion $(uW/E,vH/E)$. The current account distinguishes fractional storage from the isotropic metric, but leaves their connection implicit.
- Explain Gaussian sigma and the practical three-sigma footprint before the denoise equations. The existing glossary calls sigma a scale parameter, but a beginner benefits from “one sigma gives weight about 0.607; three sigma gives about 0.011 before normalisation”.
- Update the introductory 20-page claim after the newly authorised expansion and PDF regeneration.

The reference already clearly distinguishes linear light, the editing power coordinate, HSV value, Oklab lightness and linear luminance. No wholesale rewrite is needed; these changes resolve local notation and explanatory gaps.

## Resolution and final validation

The final revision addresses all three findings: Dehaze now explicitly consumes the Texture/Clarity-adjusted colour, its Oklab multiplier is written in full, and the shared Presence bypass is stated before the controls. Added the unit-luminance ratio explanation, a numerical Dehaze example, explicit mask coordinate conversion and Gaussian-width explanation.

Additional expansions explain the four brightness coordinates, white-balance gains, capture versus development exposure, curve reading, hue shift versus grading, overlapping local adjustments, linear-light interpolation and output choices. Subsections are consistently numbered and the glossary is alphabetised. Removed the Markdown/PDF generation instructions from the reference at the user's request.

Regenerated the PDF at 22 A4 pages, using the newly authorised longer length. All relative links resolve; representative pages were visually checked, and no text blocks extend near the page edge. No production code changed.
