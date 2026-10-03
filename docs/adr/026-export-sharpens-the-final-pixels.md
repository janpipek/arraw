# Export sharpens the final pixels

Photographers sharpen for the output, not for the screen: the amount that suits
a downsized JPEG is not the one that suits the full-size frame being edited.
Lightroom calls this output sharpening and applies it only on export.

## Decision

**`ExportOptions::sharpening`** is an integer 0-100; 0 is off and the export is
bit-identical to one without the option. Outside the range `exportImage` throws
`std::invalid_argument`. The CLI spells it `--sharpen N` (a bad value is a usage
error, like `--quality`), Python `save(..., sharpening=N)`.

**Where in the chain.** Sharpening is the last image operation: after the
conversion into the output encoding and before quantisation to 8 or 16 bits.
The mask therefore sees the transfer-encoded, perceptual values the viewer
will see, which is what output sharpening is tuned against, and it works in
float so quantisation does not eat small overshoots. With sharpening on, the
conversion goes to `RGBA32FPx4` first; with it off the existing path is
untouched.

**The mask.** An unsharp mask: `v + strength * (v - blur(v))` per colour
channel, with a separable Gaussian of sigma 1.0 output pixel (kernel radius
`ceil(3 sigma)` = 3, edges clamp), then clamped to the encoding's [0, 1].
`strength = amount / 100 * 1.5`. Sigma 1 is the fine-detail radius that
survives JPEG and suits typical screen output; export does not know viewing
distance. 1.5 at amount 100 is clearly crisp without heavy halos; 1.0 is
barely visible on downsized output and 2 or more rings on hard edges.

**Alpha** is not sharpened. Translucent pixels are sharpened premultiplied
and unpremultiplied afterwards, so colour does not bleed from or into
transparent neighbours; alpha 0 stays colour 0.

**Export only.** The preview does not show it, as with Lightroom's output
sharpening; what the viewer shows is the develop result, and sharpening is
applied when pixels leave for a file.
