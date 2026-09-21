# Linear Rec.2020 is the working space

`ColorEncoding` began with an enumerator called `LinearWorking`: a role among
identities, naming the space development happens in without saying which space
that was. "Convert to the working space" therefore had no meaning, and imported
images kept whatever encoding they arrived in. Development maths — exposure,
blending, masking — is only physically sensible on linear samples, and a
working gamut narrower than the output gamuts would clip saturated colour
before the user ever chose an output.

## Decision

The working space is linear Rec.2020: Rec.2020 primaries with a linear transfer
function, defined once in `src/core/ColorSpaces.h` and used by both the import
and the export direction.

Rec.2020 is chosen over ProPhoto RGB, which the previous implementation also
rejected (ADR 0001 on the `main` branch). ProPhoto has two imaginary primaries,
so saturation and hue maths can produce physically meaningless samples whose
artifacts appear only after the output transform. Rec.2020's primaries are all
real and contain both supported output gamuts, Display P3 and Adobe RGB.

The space and its role get separate names. `ColorEncoding::LinearRec2020` names
the colour space, consistently with `Srgb`, `DisplayP3`, and `AdobeRgb` beside
it; `arraw::workingEncoding` names the role it currently plays. Code meaning
"whatever we develop in" refers to the constant, so changing the working space
is a one-line change and the compiler distinguishes the two intents. They will
diverge once RAW decode arrives, where LibRaw's `output_color` setting is a
statement about Rec.2020 rather than about a working space.

Import converts every decoded image into this space, honouring an embedded
profile and assuming sRGB for an untagged file. Because linearised samples need
headroom in the shadows, imported buffers are at least sixteen bits per
channel. Export accepts working-space buffers and converts them to the
requested output encoding; the working encoding is rejected as an *output* one,
being an internal representation rather than a delivery format.

Camera input profiles (DCP/ICC) remain out of scope, as on `main`.

## Consequences

- An eight-bit sRGB JPEG occupies four times its decoded size once imported.
- Export is no longer byte-exact for a load/save round trip: the samples pass
  through a colour conversion in each direction.
- Adding an output encoding means one entry in `colorSpaceFor`.
- Whether the same space suits RAW decode is a separate decision, to be taken
  when LibRaw arrives; `main` sets LibRaw's `output_color=8` for exactly this.
