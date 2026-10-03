# Colour adjustments follow the tone chain

Saturation, Vibrance, HSL and Black & White are the pointwise colour controls.
They are leaves of the settings table like the tone ones (ADR 008), resolve
into the plan's pointwise group (ADR 011), and run on the CPU and the GPU from
the same maths. The maths is `main`'s, ported; only its structure is new.

## Decision

**Settings and keys.** All are domain units, -100 to 100, default 0, Applicable
to every photograph and affecting the pointwise stage.

- `ColorSettings`: `saturation`, `vibrance` (group Color).
- `HslSettings`: eight `HueBand`s (red, orange, yellow, green, aqua, blue,
  purple, magenta), each with hue, saturation and luminance. Keys are
  `hueRed`, `saturationRed`, `luminanceRed` and so on (group Hsl).
- `BlackAndWhiteSettings`: `convertToGrayscale` and eight weights `grayRed`
  to `grayMagenta` (group BlackAndWhite).

**Lightroom alignment.** The bands, the ranges and the names follow
Lightroom's HSL and Black & White panels, so that sidecars and photographers'
habits transfer. A full hue shift turns thirty degrees, a full saturation shift
is half the band's saturation, a full luminance shift is half of white (values
above white stay recoverable). The plan carries HSL values on a -1 to 1 scale
and Saturation and Vibrance likewise, with the gray weights left at -100 to
100, as `main` did.

**Where in the chain.** After the shoulder, in `main`'s order:
`rollHighlights`, then, if `convertToGrayscale`, Black & White, else HSL,
Saturation and Vibrance. The colour controls act on what the viewer will see,
and Saturation and Vibrance read chroma in Oklab, where a colour the shoulder
has just faded toward white should be judged as it now is. Running them before
the shoulder would let the shoulder's own desaturation undo a boost. ADR 010
said the shoulder ends the chain; it ends the tone chain, and a dated note
there says so.

**Oklab for Saturation and Vibrance.** Uniform scaling of Oklab chroma holds
lightness and hue, which an RGB or HSV scale does not. Saturation scales
chroma by `1 + amount`; Vibrance by `1 + amount * 0.2 / (0.2 + chroma)`, so
muted colours move most and vivid ones least without ever not moving. Oklab is
defined from linear Rec.709; the working space is Rec.2020, so each call
converts through `main`'s mutually inverse matrices. Negative channels, which
a camera matrix can produce, keep their sign through the cube root.

**HSV bands for HSL and Black & White.** HSL works on HSV as `main` did.
Each band weighs in with a smoothstep over sixty degrees either side of its
centre, around the wheel, and the weights are normalised, so a colour between
two bands takes a blend, and a pure colour is diluted by its neighbours' zero
shifts. Black & White uses the same bands and weights on the colour's
saturation, so a neutral is never moved and an all-zero mix is plain
luminance.

**Black & White replaces the colour controls.** With the switch on, the
colour is made grey and Saturation, Vibrance and HSL are not run: a grey
photograph has nothing for them to act on. Their values are kept, so turning
the switch off brings them back.

**Defaults are identity.** Each control left at zero resolves to a flag that is
off, and `adjustColor` skips it, so default settings leave every pixel
bit-for-bit as it was and cost three branches. Out-of-range values are clamped
in the plan, as for tone (ADR 008).

**Mirrors.** `ColorAdjustments.cpp` is the CPU source of truth;
`develop.frag` mirrors it function for function, with exact-semantics
`max`, `min` and `cbrt`. The uniform block carries each eight-band set as two
`vec4`s, and the pointwise probe gains an `AfterShoulder` stop so that parity
failures can be placed before or after the colour block.
`developPixel` is no longer `constexpr`: it takes cube roots.

## Consequences

- The pointwise group of the plan, and so the checkpoint boundary (ADR 011),
  includes these settings: moving a saturation slider recomputes the pointwise
  pass and everything after it, and nothing before.
- The develop panel shows them: a Treatment row (Colour or B&W, which edits
  `convertToGrayscale`), then Color, HSL (Hue, Saturation and Luminance pages,
  a view choice rather than an edit) and the Black & White mixer. B&W hides Color and HSL
  and shows the mix; White Balance and Tone never hide. They are also
  reachable through the CLI, sidecars, JSON and Python.
- Colour Grading and a tone curve, which `main` ran in this part of the chain,
  are not here, and will follow the same pattern.
