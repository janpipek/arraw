# arraw

A lightweight RAW photo editor: open a folder, edit non-destructively with a
real-time GPU preview, export. Not a DAM, not a cataloguing tool.

## Language

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
