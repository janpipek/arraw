# Photo-development document review: beginner reader

Date: 2026-10-14\
Reviewer: GPT-6, assessing the requested perspective of a reader new to both photography and mathematics\
Document: [photo-development.md](../photo-development.md)\
Scope: Full document; comprehension, prerequisites and navigation. This review does not validate implementation fidelity or claim human photography experience.

## Verdict

The document is a strong reference with unusually useful slider examples, but is not yet independently approachable by a beginner. It explains many subtle exceptions before establishing the everyday visual meaning of the operation. Keep the technical material; a compact reading path, notation key and several visual examples would substantially improve access within the remaining page budget.

## Findings, in priority order

### 1. High: the first reading path assumes concepts that it promises to teach

**Location:** Chapter 1; chapters 2–3, especially §2.1 and the colour-space ledger.

The guide sends new readers straight into camera-native RGB, colour profiles, working gamuts and transfer functions. A beginner has no initial picture of why a RAW image needs development, how a sensor mosaic differs from an ordinary image, or what an RGB channel represents. Later glossary definitions are correct but too compact to supply that missing mental model. “Primaries and white point define RGB colour meaning” also assumes the reader already understands both terms.

**Revision:** Add a short “First reading” paragraph before the pipeline: the sensor records differently filtered light; demosaicing estimates full-colour pixels; white balance corrects the light; tone/colour tools shape appearance; export translates the result for a viewer. Follow with a three-sentence colour-space analogy: RGB numbers are coordinates, the colour space supplies their meaning, and conversion preserves intended colour while changing those coordinates. Define primaries and white point in the glossary. Mark the long WB/matrix and interpolation derivations as optional on a first read.

### 2. High: formulas and pseudocode have no notation key

**Location:** §2.1 onwards; examples include `diag` in §3.3, `fract` and sums in chapter 5, `⊙` in chapter 7 and `Mᵀ` in chapter 8.

A beginner cannot reliably distinguish a slider input from an intermediate value, or interpret operators such as clamp, max, subscripts, superscripts, vector multiplication and summation. Some symbols change role across sections: `S` is smoothstep and HSV saturation; `L` is Oklab lightness and a log-luminance source measurement; `p` is a tone coordinate and a pixel in local-adjustment notation. The local definitions help experts but increase rereading for a novice.

**Revision:** Add a compact notation box: a prime means “after this step”; bold triples are R/G/B together; clamp limits a value; max chooses the greater value; a sum adds band/pixel contributions; powers and logs describe scaling. Define `fract`, `diag`, elementwise multiplication and transpose at their first use. Explicitly say symbols are local to their section, and distinguish “smoothstep S” from “HSV saturation S” in prose. A small symbol table is more useful here than adding derivations.

### 3. Medium: the pipeline order and teaching order diverge without a warning

**Location:** Chapter 2 diagram versus chapters 4–7.

The diagram places denoising first, but the detailed explanation follows tone and colour. A novice reading sequentially can mistake chapter order for execution order, particularly because the document correctly stresses that order matters. “Presence context” is introduced in the diagram before Presence receives a visual definition.

**Revision:** State directly under the diagram that the following chapters group controls by subject rather than execution order, and that arrows are the authoritative order. Define Presence there as the shared label for Texture, Clarity and Dehaze. Link stage names to their chapters in the Markdown.

### 4. Medium: the strongest user-facing explanations would benefit from one connected example

**Location:** Chapter 4 regional weights; chapter 5 B/W and split toning.

The tone table and isolated Highlights example are excellent, but readers must independently connect numbers to subjects in a photograph. B/W has a precise formula but no demonstration of how changing the red/orange mix separates a subject from its background. Split toning describes vector offsets and Gaussian bells without a simple “what would I see?” example. These are key controls specifically requested by the user.

**Revision:** Add three brief examples: Highlights darkens a bright cloud without treating every pixel equally; raising a red/orange B/W mix can lighten a red/orange subject relative to foliage, depending on hue overlap and saturation; blue shadows plus warm highlights can colour dark and light neutral regions while leaving the brightest whites protected. Include one numerical B/W example using a stated effective mix and saturation, avoiding the misleading assumption that an individual band acts alone. For grading, explain that setting a hue with zero zone saturation has no visible effect.

### 5. Medium: diagrams do not yet show the actual shapes that dominate comprehension

**Location:** Chapter 4 tone-window text diagram; chapter 5 hue-band and grading descriptions.

The pipeline diagram communicates sequence well. The tone-window ASCII diagram conveys positions but not the strength of a curved falloff; newcomers still need to decode the table or formulas. Hue wrap-around and overlapping tonal zones are spatial ideas that are hard to infer from prose alone.

**Revision:** Use compact static plots that survive PDF conversion: the four tone weights against tone coordinate, with a secondary row of representative linear-luminance values; and the three grading weights against tone coordinate. A small hue-wheel sketch or one-line circular sequence with overlap can show why red also receives orange/magenta weights. Label every axis and state that weight 1 means full coefficient, not “the pixel becomes white”. These can replace some explanatory repetition to preserve the page limit.

### 6. Low: glossary and section navigation are harder than necessary

**Location:** Reading guide; chapters 4–8; chapter 10.

The reading guide says terminology is chapter 10, but there are no direct links to relevant entries. Most subsections outside chapters 2, 3 and 9 are unnumbered, making precise references inconsistent. NaN, infinity, logarithm, interpolation, histogram, sigma and pixel neighbourhood appear before a novice-friendly explanation; sigma's current glossary definition is particularly terse.

**Revision:** Number subsections consistently or add stable heading links. Link the first use of crucial terms to the glossary. Add compact entries for interpolation (estimating between samples), neighbourhood (nearby pixels used by a filter), histogram (counts of pixels by brightness/channel value), and NaN/nonfinite (invalid or unbounded arithmetic values, mainly implementation safeguards). Expand sigma as the width of a blur's weighting bell; most readers can skip numerical edge-case sentences on first reading.

## Strengths worth preserving

- The explicit distinction between slider numbers, luminance and tone coordinates prevents the common “50 means 50% brightness” misunderstanding.
- The Highlights weight table, exposure gain examples and B/W saturation-dependent explanation give readers concrete footholds.
- The document consistently separates operations that reveal existing detail from impossible recovery of clipped measurements.
- The distinction between channel curves, hue bands, global saturation and grading is clear once their prerequisites are understood.
- The glossary, source links and external reading give a good foundation for a reference that can serve increasingly experienced readers.

## Recommended acceptance check after revision

A new reader should be able to explain, without evaluating a formula, why Highlights differs from Exposure, why moving a red curve differs from moving a red hue band, why B/W colour sliders still matter, and why selecting a grading hue alone need not change an image. They should also be able to identify where formulas become optional and where to look up every operator needed for a worked example.
