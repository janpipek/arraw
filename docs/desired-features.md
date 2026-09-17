# arraw — a RAW editor for photographers

(This is document was written by claude based on the previous iteration of this project, Sep 2026)

> A lightweight, cross-platform RAW developer with a Lightroom-style workflow: open a
> folder, cull, develop non-destructively, export. No catalogue, no import step, no
> subscription.

Your edits are never baked into the file. They live in an `.xmp` sidecar next to the RAW,
written in the same `crs:` dialect Lightroom uses — so ratings, colour labels and most
develop settings survive a round-trip to and from Lightroom. Delete the sidecar and you
have your untouched negative back.

---

## The workflow

**1. Open.** Point arraw at a single file or a whole folder. There is nothing to import
and nothing to wait for — the folder *is* the session. Arrow keys walk the filmstrip;
RAW+JPEG pairs are shown as one frame with the RAW as the primary.

**2. Cull.** Rate `1`–`5`, `0` to clear, `X` to reject; assign one of five colour labels.
Filter the filmstrip by rating or label to work only on the keepers. Marks are written to
the sidecar, not to a database, so they travel with the photo.

**3. Develop.** Every slider redraws the full preview instantly — there is no "rendering"
pause between moving a control and seeing the result. Hold `\` for a before/after flip,
`J` for a clipping overlay. Every change goes on one undo stack (`Ctrl+Z`), with a
slider-drag counting as a single step rather than a hundred.

**4. Export.** JPEG, PNG, or TIFF (8- or 16-bit), to sRGB, Display P3 or Adobe RGB with
the ICC profile embedded. Export runs off the interface, so you can keep editing while a
batch writes out.

---

## Adjustments

Controls are grouped the way you'd expect. A **Colour ↔ Black & White** switch sits at the
top: turning on B&W hides the colour controls and reveals the mixer.

| Group | What you get |
|:---|:---|
| **White Balance** | Temperature (2000–12000 K) and Tint, camera presets, and an eyedropper for picking a neutral |
| **Tone** | Exposure (±5 EV), Contrast, Highlights, Shadows, Whites, Blacks, plus Filmic Highlights — a soft shoulder that rolls speculars toward white instead of clipping them flat |
| **Tone Curve** | A Luma curve and independent R/G/B curves, drawn over the histogram |
| **Color** | Saturation and Vibrance, plus HSL — hue, saturation and luminance across 8 colour ranges |
| **Black & White** | An 8-band hue mixer: each original colour is weighted into its own grey, the way a coloured filter over B&W film behaves. Not a flat desaturation |
| **Colour Grading** | Independent hue + saturation for shadows, midtones and highlights, with Balance and Blending. Works over colour *and* B&W images |
| **Detail** | Demosaic choice; Texture, Clarity, Dehaze, Sharpen; separate Luminance noise (amount/detail) and Colour noise (strength/smoothness) reduction, both edge-aware |
| **Geometry** | 90° rotation and flips, Straighten (±45°), Crop with aspect presets |
| **Lens Corrections** | Profile-driven distortion, vignetting and chromatic-aberration correction, from the lensfun database or the profile embedded in the file |
| **Effects** | Post-crop vignette (amount / midpoint / feather) and film grain (amount / size / roughness) |

### Local adjustments

Up to 16 masked adjustments per image, each with its own tonal and colour deltas:

- **Linear** — a graduated fade across the frame, for skies and foregrounds.
- **Radial** — an oval, for vignetting attention onto a subject.
- **Brush** — a freehand stencil you paint on directly. It is anchored to the photo, so it
  stays put when you zoom, pan or crop.

**Spot removal** is separate: a clone-based tool for sensor dust and blemishes.

---

## Colour and output

arraw works internally in a wide, linear space and only converts at the very end, so
heavy edits don't collapse saturated colours early in the chain. Practically:

- **Embedded profiles are honoured** on load, and your monitor's ICC profile is used for
  display.
- The preview, the export, and the command line all run the **same** processing — what
  you approve on screen is what lands in the file.

---

## Repeating yourself less

- **Copy / paste settings** between images, with a checklist of which groups to carry over.
- **Batch paste and batch export** across a multi-selection.
- **Develop presets** — named, *partial* bundles (e.g. just the tone curve and grain),
  saved by name and manageable in one dialog.
- **Snapshots** — named A/B versions of an image's full develop state, stored per image, so
  you can park a look and try another.
- **History** — every edit step in the current session, to step back to.

### Without opening the window

Arraw is also a command, running the identical pipeline headless:

```bash
arraw-cli export *.arw -o out/   # render every file through its own sidecar
arraw-cli preset list            # list, show, or apply saved presets
arraw-cli info photo.arw         # EXIF and edit state, read-only
```

Useful for overnight batches, or for re-exporting a shoot in a different size or profile
without touching the edits.

---

## The practical details

- **RAW formats:** CR2, CR3, NEF, ARW, DNG, RAF, ORF, RW2, PEF, SRW.
- **Platforms:** Linux (AppImage / Fedora RPM), Windows (installer or portable ZIP),
  macOS from source.
- **Metadata:** the Info panel shows read-only camera EXIF alongside editable Title,
  Caption, Keywords, Creator and Copyright. On export you choose per group what gets
  embedded — GPS is off by default.
- **Working comfortably:** full-screen (`F11`), lights-out (`F12`) to hide every panel,
  a collapsible adjustment dock, zoom from 0.05× to 32× with scroll, pan with Alt+drag.
  Thumbnails and decoded frames are cached, so a second pass through a folder is quick.

## What it is not

It is not a digital asset manager. There is no catalogue, no cross-folder search, no
keyword hierarchy, no collections — culling is per-folder triage while you edit. It does
not do panoramas, HDR merging, or layered compositing.

