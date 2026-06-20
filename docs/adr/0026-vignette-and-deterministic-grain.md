# Vignette is crop-relative; Grain is deterministic perceptual texture

Post-crop Vignette and Grain are the non-spatial Effects controls: both remain
per-pixel GPU adjustments in the main render pass, preview live, and are included
in thumbnails, histograms, clipping evaluation, and export. Vignette runs after
local tone/colour work as a hue-preserving exposure falloff in final crop-frame
coordinates. Grain runs after Vignette by temporarily encoding working values
with the sRGB transfer curve, adding monochromatic zero-mean texture, and decoding
back to linear before the existing display or lcms2 output transform.

## Controls

- **Vignette:** Amount -100..100 maps to -2..+2 EV at maximum falloff; negative
  darkens and positive lightens. Midpoint and Feather are 0..100. The neutral
  defaults are Amount 0, Midpoint 50, Feather 50. The falloff is a centred ellipse
  fitted to the cropped frame; higher Midpoint moves the transition outward, while
  Feather ranges from a hard boundary to the broadest smooth transition.
- **Grain:** Amount 0..100 maps linearly to 0..0.08 standard deviation in encoded
  values. Size 0..100 maps logarithmically to approximately 0.5..4 pixels per 2048
  pixels of crop-edge length. Roughness 0..100 blends multiple noise scales from
  uniform/fine to irregular/clustered. Defaults are Amount 0, Size 50, Roughness 50.

Grain is a continuous image-relative noise field keyed by cropped-frame coordinates
and a hidden per-image seed. This keeps the pattern fixed across repaint, pan, zoom,
reopen, and output resolutions; a low-resolution preview can soften fine Grain but
must not rearrange it. The seed is generated when Grain is first enabled, retained
when Amount returns to zero, and cleared by Reset All. Copy Settings and Develop
Presets transfer the visible Effects controls but retain or generate the target
image's own seed.

## Considered Options

- **Add Grain after the exported output transform.** This makes output-pixel sizing
  literal, but duplicates the adjustment on the CPU, breaks the shader-as-source-of-
  truth invariant, and requires separate implementations per output path. Rejected.
- **Add Grain directly in linear light.** Cheap, but its contrast is visually wrong
  and varies strongly with scene luminance. Rejected.
- **Use fresh random noise on every render.** Simple, but the image shimmers and
  preview cannot correspond to export. Rejected.
- **Use a fixed pattern for every image.** Reproducible without state, but repeated
  texture becomes visible across a batch. Rejected in favour of a persisted seed.

## Consequences

The vertex shader must pass unrotated crop-frame coordinates to the fragment shader
in addition to source sampling coordinates. The uniform block remains byte-identical
in `image.vert`, `image.frag`, and `RendererCore::Ubuf`. The six visible controls use
Lightroom-compatible `crs:` fields; the reproducibility-only seed uses
`arraw:GrainSeed`. Effects becomes the eighth Develop Group, but its per-image seed
is deliberately excluded from copy/paste and preset serialization.
