# Lens corrections: hand check on one Canon 80D CR2 (step 11, partial)

Date 2026-10-18, branch `lens-corrections` (2150c30). Sample: `IMG_3659.CR2` +
in-camera `IMG_3659.JPG` (not committed; kept in `/workspace/samples/`, excluded
through `.git/info/exclude`). Canon EOS 80D, 6024x4020, 42 mm, f/9, ISO 125.
Lens: Tamron SP AF 17-50mm f/2.8 XR Di II LD Aspherical (IF), non-VC
(user-confirmed). lensfun 0.3.4, database stamp 2022-02-20.

## Done

- **Match (items 1, 2, 7).** The EXIF lens string is only `17-50mm` (no maker, no
  aperture). `arraw-cli info` matched "Tamron SP AF 17-50mm f/2.8 XR Di II LD
  Aspherical (IF)", crop 1.613 (the camera's), source "matched". The user confirms
  this is the right lens (exiftool: "Lens ID: Tamron AF 17-50mm f/2.8 Di-II LD
  Aspherical"). Why it worked, measured with lensfun's own scoring: the bare name
  `17-50mm` scores 39/33/33/28 for Sigma, Tamron A16, Tamron SP IF and Tamron SP VC,
  all under our minimum of 40, so it names no lens. The second name, from the Canon
  lens-type ID (Exiv2: `Exif.CanonCs.LensType` = 161, "Tamron AF 17-50mm f/2.8
  Di-II LD Aspherical"), scores 83 for the non-VC and 77 for the VC. The match is by
  design, not luck, but the margin is thin: 83 - 77 = 6 against an ambiguity margin
  of 5. The VC is a different Canon lens ID, so the ID could separate them with
  certainty; fuzzy scores do not.
- **Alignment of the reference.** The camera JPEG (6000x4000) is the centre crop
  of the RAW frame at offset (12, 10). Against our uncorrected render it differs by at
  most 0.04 px across 25 patches: the camera's in-camera lens corrections were off,
  so the JPEG is an uncorrected reference and cannot validate the correction. It
  does confirm decode, orientation and crop geometry.
- **Distortion (item 3), numbers only.** With all corrections on, content moves
  radially: 0 px at the centre, 6 to 8 px at 30% of the frame, 12 to 19 px at the
  frame edges and corners (patch phase correlation, 384 px patches, 25 positions;
  peaks 0.1 to 0.3, left/right and top/bottom symmetric). The size is plausible for
  this lens at 42 mm; its direction and amount are lensfun's calibration, which we
  do not test.
- **Vignetting (item 4), weak evidence.** In a sky band (y 100-500) the corners rise
  by 9% against 2% at the centre (on/off). Stopped down to f/9, so the effect is
  small; the sky has its own gradient, so evenness cannot be judged. Needs a
  wide-open shot of an even surface.

## Not done (needs the user)

- Visual judgement of straight lines and lateral CA at the corners, with crops
  (`crop_*.jpg` in the scratchpad `hc` pair).
- Items 5 (CA), 6 (GUI toggle, crop, masks, brush in place), 8 (clipped corner).
- A wide-open shot for vignetting; a second lens, if any.
- `exiftool -v3` was not available; the maker-note check for coefficients (only
  switches, not data, in CR2) stays unconfirmed on this file.

## Findings

1. Export with all three corrections takes 36 s CPU at 24 MP on this machine
   (software rasteriser, shared CPU). Nothing to fix now; GPU parity is measured
   on lavapipe only.
2. The Canon lens-type name picks the lens by a 6-point fuzzy margin (see above).
   Candidate fix: when the name comes from the maker note, drop entries that carry
   distinguishing tokens the name lacks ("VC", "OS", "Macro"), or prefer an entry
   whose name contains all the name's tokens.

## Second file: IMG_2610 (17 mm, f/2.8, ISO 800)

Same body and lens; same match (Tamron SP AF 17-50mm f/2.8 XR Di II LD Aspherical
(IF), through the Canon lens-type name). Lens corrections were off in the camera:
the JPEG matches our uncorrected render to 0.05 px again.

- **Distortion, against lensfun's own model.** Predicted per patch with `lfModifier`
  (entry crop 1.53, focal 17, f/2.8) and corrected for the fill zoom of ADR 050: the
  measured shifts agree within about 1 px at the patches with a clear correlation peak
  (e.g. left 30% mid-row: predicted +21.7, measured +19.0 before the zoom, +19 after;
  corners -29/-19 predicted, -34/-22 measured, zoom included). So the kernel does what
  lensfun's model says. The fill zoom is 0.24% at 17 mm. The same analysis on the first
  file (42 mm) gives a 0.9% zoom, fitted from the mid-row shifts, which matches the
  measured shifts to within 1 px: lensfun's distortion there is only about 3 px, and
  the zoom (from corners moving 10 px inward) dominates.
- **Straightness: not decisive.** Five teal poles at the right edge (x 90 to 98%):
  tilt 0.7 to 1.2 degrees before and after, the same (a camera roll, not the lens). The
  distortion bows a 1000 px pole by only about 5 px at this position, which is
  subtle by eye. A long straight line crossing a corner would show more.
- **Vignetting: inconclusive, possibly worrying.** Sky band y 40-240: the uncorrected
  luma is flat across x (0.684 to 0.687) but the correction raises it to 1.05 at
  x = 3800, 1.18 at 5300 and 1.27 at 5700 (top-right corner). Either the sky really
  falls off towards the corner (then the correction is right) or lensfun's calibration
  overcorrects this copy. Needs a wide-open shot of an even surface (wall, overcast).
- Crops and renders for the user are in `samples/lens-check/` (`c2_*.jpg`,
  `IMG_2610_corrections-*.png`).
