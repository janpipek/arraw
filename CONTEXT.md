# arraw

A lightweight, cross-platform RAW photo editor with a Lightroom-style development
workflow: open a folder, edit non-destructively with a real-time GPU preview,
export. Not a DAM, not a catalogue.

## Language

**Working color space**:
The color space all pixels live in from RAW decode until the final display/output
transform: linear-light Rec.2020 primaries. Every adjustment operates in this space.
_Avoid_: internal color space, pipeline color space

**Display transform**:
The conversion from working color space to what the monitor expects, applied as the
last step of preview rendering.
_Avoid_: gamma step, sRGB step

**Output transform**:
The conversion from working color space to the color profile chosen for an exported
file (sRGB, Display P3, AdobeRGB).
_Avoid_: export color conversion

**Soft-proofing**:
A preview mode that temporarily replaces the display transform with a simulation of
a printer/paper profile, so the screen predicts the print.
_Avoid_: print preview, proof mode

**Tone Curve**:
A user-editable remapping of gamma-encoded (display-space) values, defined by
control points in [0,1]². One Luma Curve plus three Channel Curves per image.
_Avoid_: curves (ambiguous with the LUT that implements them)

**Luma Curve**:
The tone curve applied to luminance; it scales RGB proportionally so hue is
preserved.
_Avoid_: master curve, RGB curve

**Channel Curve**:
A tone curve applied to a single R, G, or B channel independently; can shift
hue by design.

**Curve Input Histogram**:
The histogram of the image as the tone curve receives it — after upstream
adjustments, gamma-encoded — drawn behind the curve so its x-axis matches the
curve's. Distinct from the panel Histogram, which shows the final image.
