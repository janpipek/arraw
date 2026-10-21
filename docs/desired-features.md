# arraw — a RAW editor for photographers

(This is document was written by claude based on the previous iteration of this project, Sep 2026)

> A lightweight, cross-platform RAW developer with a Lightroom-style workflow: open a
> folder, cull, develop non-destructively, export. No catalogue, no import step, no
> subscription.

Your edits are never baked into the file. They live in an `.xmp` sidecar next to the RAW,
named as Lightroom names it. Ratings and colour labels are the standard `xmp:` ones, so
they travel to and from Lightroom today; develop settings are in arraw's own namespace for
now, and will be mapped to Lightroom's `crs:` dialect so that most of them survive the
round-trip too. Whatever else the sidecar holds, arraw leaves alone. Delete the sidecar and
you have your untouched negative back.

---

## The workflow

**1. Open.** Point arraw at a single file or a whole folder. There is nothing to import
and nothing to wait for — the folder *is* the session. Arrow keys walk the filmstrip;
RAW+JPEG pairs are shown as one frame with the RAW as the primary.
"Include subfolders" lists the whole tree, however large, in the background: the strip fills
in while the window stays usable, and a change on disk updates only the folder that changed
(`F5` re-reads the folder; a notice says when a huge tree has folders that are not watched).

**2. Cull.** Rate `1`–`5`, `0` to clear, `X` to reject; assign one of five colour labels.
Filter the filmstrip by rating or label to work only on the keepers. Marks are written to
the sidecar, not to a database, so they travel with the photo.

**3. Develop.** Every slider redraws the full preview instantly — there is no "rendering"
pause between moving a control and seeing the result. Hold `\` for a before/after flip,
`J` for a clipping overlay on the picture, next to a histogram in the develop dock. Every change goes on one undo stack (`Ctrl+Z`), with a
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
| **Negative** | Demosaic choice (Linear, VNG, PPG, AHD, DCB, DHT, AAHD) for a RAW. It re-decodes the file, so it is kept apart from the develop sliders; it is saved, undoable, and copied and pasted with the look |
| **Detail** | Texture, Clarity, Dehaze, Sharpen; separate Luminance noise (amount/detail) and Colour noise (strength/smoothness) reduction, both edge-aware |
| **Geometry** | 90° rotation and flips, Straighten (±45°), Crop with aspect presets |
| **Lens Corrections** | Profile-driven distortion, vignetting and chromatic-aberration correction, from the lensfun database, matched from the file's EXIF and recorded with the edit, off by default and for a RAW only; profiles embedded in the file are a later option |
| **Effects** | Post-crop vignette (amount / midpoint / feather) and film grain (amount / size / roughness) |

### The histogram

A histogram sits at the top of the develop dock, above the Adjustments and Masks tabs. It can
be collapsed, and it remembers whether it was.

- **It counts the picture as shown.** The whole cropped frame in the output colour space
  (sRGB today), after every edit, whatever the zoom or pan. In the crop mode it counts the
  crop being drawn. Red, green and blue are drawn over each other, or switch to **Luma**.
- **The scale is linear with a soft cap.** A spike at black or white does not flatten the rest:
  the height follows the tallest ordinary bins, and the bins that are cut off get a line at the
  top. Pure black and pure white each get a marker of their own at the side of the plot.
- **Clipping warnings.** A triangle at each top corner lights up when pixels are clipped (any of
  R, G, B at 0 or 255 in the shown 8-bit picture), in the colour of the channels that clip.
  Click one to paint those pixels on the photograph, blue for shadows and red for highlights;
  `J` turns both on, or both off. The choice is remembered between sessions.
- **A readout** under the plot gives the R, G, B percentages of the pixel under the pointer.

The count runs on a thread of its own, so it never holds up a render. Outside the window, Python
has `arraw.output_histogram(image)`; the CLI's `info` is unchanged.

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
- **Export presets** — named, whole export settings (format, profile, size, quality,
  metadata), kept in one file that the app, the command line and Python all read. Pick one in
  the export dialog, or from **Export with Preset** in the film strip and the File menu to export
  without being asked; save, update, rename and delete them in the dialog. Four built-in presets
  (web, social, full-size JPEG, print TIFF) come with arraw.
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
arraw-cli export-preset list     # list or show the saved export presets
arraw-cli export *.arw -o out/ --export-preset "Web 2048 px"
arraw-cli info photo.arw         # size, encoding, EXIF, sidecar marks and edits, read-only
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

