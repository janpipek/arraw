# arraw

A lightweight, cross-platform RAW photo editor with a Lightroom-style development
workflow: open a folder, edit non-destructively, export. Not a DAM, not a catalogue.

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
