# Architecture Decision Records

One file per hard-to-reverse decision: the context that forced it, what was
decided, the options rejected, and the consequences. ADRs are **append-only** —
a decision that changes is superseded by a new record (with a pointer added to
the old one), never rewritten in place.

Related documents:

- [CONTEXT.md](../../CONTEXT.md) — the domain glossary. ADRs cite terms as
  `[[Double Bracketed]]` links into it; new vocabulary is defined there, not here.
- [DESIGN.md](../../DESIGN.md) — the current architecture and pixel pipeline.

These records **are** the project's history: read in number order they are the
sequence in which arraw was built, each with the context that forced it. There is
no separate roadmap or milestone document to keep in sync.

## Conventions

- Filename: `NNNN-kebab-case-title.md`, `NNNN` being the next free number.
  **Check the highest number in this directory before claiming one** — 0046 and
  0049 were each briefly issued twice; the later record of each pair was
  renumbered to 0054 and 0055.
- The `#` heading states the decision as a sentence, not a topic.
- Source comments cite records as `docs/adr/NNNN`, so a number is a permanent
  identifier once merged.

## Index

### Colour and the pixel pipeline

| # | Decision |
|---|---|
| [0001](0001-linear-rec2020-working-space.md) | Linear Rec.2020 as the working color space |
| [0002](0002-shader-adjustments-cpu-encode.md) | Shader owns adjustments; color encode is a separate CPU stage (lcms2) |
| [0003](0003-tone-curve-in-gamma-space.md) | Tone curve operates on gamma-encoded values |
| [0009](0009-clipping-warning-srgb-relative.md) | Clipping warning is sRGB-relative and computed once at the encode stage |
| [0025](0025-white-balance-multiplicative-gain.md) | White balance is a multiplicative, blackbody-derived channel gain |
| [0033](0033-perceptual-basic-tone-and-recoverable-headroom.md) | Basic Tone is perceptual, endpoint-aware, and keeps recoverable headroom |
| [0039](0039-perceptual-colour-oklab.md) | Saturation and Vibrance operate in Oklab |
| [0040](0040-filmic-highlights-shoulder-path-to-white.md) | Filmic Highlights: a shoulder with a path to white, last in the develop chain |
| [0048](0048-black-and-white-treatment-hue-mixer.md) | Black & White: a treatment with a hue-aware mixer, not a desaturation |
| [0052](0052-colour-grading-three-zone-toning.md) | Colour Grading: three-zone hue/saturation toning, colour or Black & White |

### Rendering and the GPU engine

| # | Decision |
|---|---|
| [0004](0004-histograms-via-gpu-readback.md) | Histograms are computed from GPU readback, not a CPU mirror |
| [0006](0006-rhi-migration-single-renderer-core.md) | RHI migration: big-bang port, qsb-baked shaders, one renderer core |
| [0011](0011-preview-blur-pass-for-clarity.md) | Spatial global adjustments add a preview context pass |
| [0034](0034-colour-noise-reduction-gpu-chroma-pre-pass.md) | Colour Noise Reduction is a cached GPU chroma pre-pass |
| [0035](0035-async-histogram-readback.md) | Histogram readback is async, recorded into the widget's own frame |
| [0045](0045-async-export-and-thumbnail-cpu-tail.md) | Export and developed-thumbnail CPU tail runs off the GUI thread |
| [0046](0046-luminance-noise-reduction-unified-edge-aware-pre-pass.md) | Luminance Noise Reduction unifies the NR pre-pass around an edge-aware luma filter |

### Develop model: geometry, masks, and corrections

| # | Decision |
|---|---|
| [0007](0007-crop-after-rotation-display-frame.md) | Crop is axis-aligned in the rotated display frame (crop after rotation) |
| [0010](0010-parametric-local-adjustments.md) | Local adjustments are parametric masks stored in an arraw-native namespace |
| [0017](0017-spot-removal-cpu-side-clone.md) | Spot removal is a CPU-side clone applied to the decoded buffer, not a shader operation |
| [0021](0021-crop-aspect-lock-lightroom-flag.md) | The crop aspect-ratio lock persists as a Lightroom flag, ratio re-derived from the rectangle |
| [0026](0026-vignette-and-deterministic-grain.md) | Vignette is crop-relative; Grain is deterministic perceptual texture |
| [0029](0029-orientation-coarse-lossless-develop-edit.md) | Orientation is a coarse, lossless develop edit on a native-orientation buffer |
| [0032](0032-lens-corrections-cpu-corrected-negative.md) | Lens corrections are a CPU apply-once step that produces a corrected negative |
| [0036](0036-demosaic-selection-redecode-via-load-path.md) | Demosaic selection re-decodes via the load path; LGPL built-ins only |
| [0047](0047-brush-mask-buffer-anchored-raster.md) | Brush masks are buffer-anchored rasters sampled at the image coordinate |

### Persistence: sidecars, presets, and metadata

| # | Decision |
|---|---|
| [0008](0008-culling-marks-in-develop-sidecar.md) | Culling marks live in the develop sidecar; saves are namespace-scoped and read-first |
| [0023](0023-develop-presets-partial-json.md) | Develop Presets are partial, arraw-native JSON — a clean break from the `crs:` sidecar |
| [0027](0027-shared-xmp-property-ownership.md) | Shared XMP sidecars are merged by property ownership |
| [0037](0037-editable-dc-user-metadata-ownership.md) | Editable descriptive User Metadata is owned in five `dc:` properties |
| [0038](0038-snapshots-arraw-native-history-session-only.md) | Snapshots persist arraw-native; History is session-only |
| [0043](0043-exported-metadata-exiv2-corrected-passthrough.md) | Exported files carry corrected-passthrough metadata, written by exiv2 |
| [0055](0055-preset-save-defaults-to-nondefault-groups.md) | Save Preset pre-checks only groups that differ from default |

### Application shell and UI

| # | Decision |
|---|---|
| [0012](0012-collapsible-adjustments-dock.md) | Adjustments dock collapses to a right-edge strip (Film Strip stays full-hide) |
| [0018](0018-multi-select-batch-paste-export.md) | Multi-select filmstrip: LR-style active/selected split for batch paste and batch export |
| [0019](0019-filmstrip-exif-tooltip-cache.md) | Filmstrip EXIF tooltips cached as JSON sidecars |
| [0020](0020-develop-session-active-image-state.md) | DevelopSession owns active image state; widgets edit and present it |
| [0024](0024-developed-thumbnail-and-decode-caches.md) | Developed thumbnails and an in-session decode cache |
| [0028](0028-lights-out-and-fullscreen-modes.md) | Two composable focus modes: OS fullscreen (F11) and lights-out hide-chrome (F12) |
| [0031](0031-neutral-dark-fusion-theme.md) | Neutral dark photographer-friendly theme via Fusion + a single-sourced palette |
| [0042](0042-film-strip-rating-and-colour-filter.md) | Filtering the film strip by rating and colour label |
| [0056](0056-zoom-presets-one-shared-list.md) | Zoom presets come from one shared list, with the preset match extracted as pure logic |

### Command-line front-end

| # | Decision |
|---|---|
| [0022](0022-headless-cli-batch-export.md) | Headless CLI batch export: windowless QRhi, the shader stays the one truth *(amended by 0049)* |
| [0049](0049-arraw-command-front-end.md) | The arraw command front-end: one grammar, a Windows pair, and the export CLI surface |
| [0050](0050-cli-output-and-exit-contract.md) | CLI output and exit-code contract: default tables, explicit `--json`, three exit tiers |
| [0051](0051-preset-cli-commands.md) | The preset command family: list, show, apply — the GUI's presets, headless |
| [0053](0053-info-cli-command.md) | The `info` command: per-file EXIF and edit-state report, read-only |

### Build, packaging, and distribution

| # | Decision |
|---|---|
| [0005](0005-golden-image-tests-tolerance-policy.md) | Golden-image tests run the real export path on any GPU, with a dual tolerance |
| [0013](0013-app-icon-svg-source-runtime-only.md) | App icon: one SVG is the source, runtime window icon now, native packaging later |
| [0014](0014-linux-distribution-appimage-ubuntu-aqt.md) | Linux distribution: a CI-built AppImage on Ubuntu 24.04, Qt 6.8 via aqtinstall |
| [0015](0015-windows-native-icon-gui-subsystem.md) | Windows native icon and GUI subsystem (resolving 0013's deferral) |
| [0016](0016-windows-installer-inno-setup.md) | Windows installer: a per-user Inno Setup `setup.exe` over the existing staging |
| [0030](0030-self-hosted-fedora-rpm.md) | Self-hosted Fedora RPMs with manual release builds |
| [0041](0041-layered-source-directories.md) | Layered source directories with the application shell at the root |
| [0044](0044-containerised-autonomous-dev-sandbox.md) | A containerised, autonomous dev sandbox for AI coding agents |
| [0054](0054-windows-package-github-actions.md) | Windows package builds in GitHub Actions |
