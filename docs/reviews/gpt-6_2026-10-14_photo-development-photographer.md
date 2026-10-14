# Review: photo-development reference from a photographer's perspective

Date: 2026-10-14\
Reviewer: GPT-6\
Document: [photo-development.md](../photo-development.md)\
Perspective: an experienced photographer who understands exposure, curves, colour and RAW editing, but does not read mathematical derivations comfortably. This assesses communication and practical interpretation, not code fidelity. No document or production changes were made.

## Overall assessment

The document is a strong implementation reference with unusually honest explanations of what slider numbers mean. Its coverage satisfies the mathematical part of the request far better than most photo-editing manuals. It is less successful as a photographic explanation: the reader often must understand an equation before learning what to look for in a photograph. I would approve it as a technical reference, with the improvements below recommended before presenting it as equally accessible to photographers.

The improvements can fit the page budget by replacing some explanatory arithmetic with short practical summaries, rather than adding a second manual. There is no need to remove the exact formulas.

## Prioritized findings

### 1. Medium: provide a photographic reading path and a compact control map

Location: scope/reading guide, lines 15–17; chapter openings throughout.

The current route sends a photographer through input, colour science and then detailed formulas. It does not answer the practical starting question: “Which section explains the change I see when I move this slider?” “Basic tone”, “Presence”, “Filmic highlights” and “grading” are navigational labels whose differences are partly what the reader needs explained.

Action: add a short table with “photographic intention / relevant controls / section”: recovering visible bright-area separation → Highlights and Filmic Highlights; lifting dark subjects → Shadows versus Blacks; separating fine texture → Texture versus Clarity; selectively changing colours → HSL versus grading; monochrome tonal separation → B/W mix. Explicitly say the technical pipeline order is not a prescribed order in which to edit a photo. This table could replace some prose in the reading guide.

### 2. Medium: translate the two highlight tools into visible outcomes before their formulas

Location: regional tone, lines 199–258; Filmic Highlights, lines 282–296; glossary, lines 757–758.

The Highlights section gives excellent weights and examples, but the subsequent Filmic discussion begins with “linear-luminance knee” and “shoulder”. A photographer can finish both without a simple answer to when one is appropriate. The Filmic colour-neutralisation equation also has a significant visible consequence: brightly coloured lights or flowers can become paler as they approach white.

Action: preface the sections with a two- or three-sentence comparison. Highlights raises or lowers a selected brightness region; Filmic Highlights progressively compresses the brightest differences and reduces colour toward white. Mention that strong compression can make bright texture look flatter and intense colours look paler. Define knee as “where compression begins” and shoulder as “the bend that progressively rolls bright values toward white” at first use. Preserve the existing caveat that neither reconstructs sensor-clipped information.

### 3. Medium: explain grading hue selection and balance through a concrete photographic example

Location: three-zone colour grading, lines 374–396.

The distinction between Oklab angles and HSV angles is valuable, but “+a toward +b” is not a usable colour picker explanation for this persona. Gaussian bells and an Oklab offset do not convey what to expect when, for example, blue is added to warm shadows. The text says the tint can reinforce or neutralise colour, but the reader must infer that adding a hue is not replacing every selected pixel with that hue.

Action: add a small worked verbal example: a cool shadow tint can cool neutral shadows, neutralise orange shadows or shift existing colours, with strength fading across zones. State which direction Balance expands the highlight treatment in plain words, and that Blending broadens overlap rather than simply weakening the treatment. Keep the current approximate hue-to-colour angle list; if space permits, make it a compact lookup table. Explicitly connect “split toning” to differently tinted shadows/highlights, including after B/W.

### 4. Medium: give the B/W colour mixer a photographic subject example

Location: B/W conversion, lines 366–372.

The formula and saturation dependence are clear technically, but the short section never directly tells a photographer why they would move a particular band. “Colour filters” in the heading can suggest simulation of physical lens filters, whereas the text explicitly describes a different weighted digital luminance rule.

Action: add: increasing the blue mix lightens blue subjects in the monochrome result; decreasing it can darken a blue sky relative to clouds; adjusting orange can change orange-toned skin relative to other subjects. Qualify these as subject-colour examples with overlapping bands and saturation-dependent strength, not universal recipes. Explain that the mixer uses original colour to choose grey brightness even though the displayed result has no colour. Either clarify the digital meaning of “colour filters” or rename the heading “B/W conversion and colour mixer”.

### 5. Medium: denoise/Presence need a short “what to inspect” explanation

Location: Noise reduction and Presence, lines 398–510; preview discussion in chapter 9.

The chapter begins with matrices, ratio decomposition and filter kernels. It defines slider numbers accurately, but gives little advice for recognising the difference between a desired result and an artefact. A photographer needs to connect luminance smoothing to fine brightness texture, colour smoothing to coloured blotches, Detail to the tradeoff between noise and edges, and Texture/Clarity to different scales of subject detail. Preview caveats are separated from the controls where they matter most.

Action: add a compact paragraph or table before the decomposition: inspect smooth shadows for noise, fine hair/fabric for detail loss, colour boundaries for colour bleeding, and high-contrast edges for excessive local contrast. Explain that higher Detail can retain both real detail and noise. Cross-reference the existing preview/export limitations so readers know a small fitted preview can conceal these effects. This need not supply fixed recommended slider values.

### 6. Low: distinguish slider percentages, mask coverage and geometric fractions consistently

Location: mask explanation, lines 518–563; relative Temperature/Tint following it.

The numerical examples are useful, especially opacity × exposure and repeated brush dabs. However, notation switches between 0–100 sliders and 0–1 quantities without a single practical bridge. A photographer may read flow 0.5 as a literal UI value, or assume feather 0.5 corresponds to half of the entire ellipse being half affected. “Coverage” is also introduced as a computational quantity rather than simply strength at a location.

Action: say once that coverage 0, 0.5 and 1 mean no, half and full contribution to the local settings; normalized flow/opacity 0.5 means 50% if the interface displays percentages. For feather, say it is the transition width toward the edge, not the overall adjustment strength. Distinguish stored engine units from displayed units where the interface differs, as already promised in the scope.

### 7. Low: output guidance names colour spaces without explaining the photographic decision

Location: display/export, lines 681–706.

The encoding and profile statements are precise. A photographer choosing an export still needs the practical meaning: a wider gamut is not automatically more accurate in every viewer, and more bits are useful for later editing rather than creating extra captured detail. This is particularly relevant because display is sRGB while export may use P3 or Adobe RGB.

Action: add a short decision note: choose the colour space expected by the recipient/workflow and preserve its profile; compare in a colour-managed application; use higher bit depth when the next editing step benefits from finer gradations. Avoid prescribing a universal print profile or implying that a wide-gamut export makes the sRGB preview a proof of every exported colour. The current lack of proofing is already disclosed and should remain visible.

## Strengths worth preserving

- The complete processing diagram explains dependencies without pretending the editing controls are independent.
- Highlights' weight table and the explicit “+50 does not mean half an EV” explanation directly answer the user's original question.
- Exposure is expressed in familiar stops, with clipping limitations stated clearly.
- The per-band hue example exposes dilution by overlapping bands instead of misleading readers with a nominal maximum shift.
- B/W is accurately distinguished from a channel average or film emulation, and grading after monochrome is made explicit.
- Vignette darkening and lightening are distinguished; the text avoids presenting the positive setting as a uniform exposure gain.
- Brush accumulation has a memorable numerical example, and preview versus export differences are acknowledged.
- The glossary, source links and external conceptual links make the document useful at multiple depths.

## Suggested minimum revision

If only a few lines can change, prioritize a photographic control map, a plain-language Highlights/Filmic comparison, and one concrete B/W and split-toning example. These would materially improve usefulness for this persona while retaining the technical depth and page budget.
