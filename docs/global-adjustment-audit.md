# GlobalAdjustment Audit

Issue 21's dead-state audit treats every field on `GlobalAdjustment` as either
consumed by the develop pipeline or intentionally excluded from a specific flow.

| Field | UI owner | Preview/export consumption | Persistence | Copy/Preset group |
|---|---|---|---|---|
| `exposure`, `contrast`, `highlights`, `shadows`, `whites`, `blacks` | `AdjustmentPanel` Tone sliders | `RendererCore::fillUbuf()` -> `image.frag` tone regions | XMP `crs:*2012` attributes | Tone |
| `temperature`, `tint` | `AdjustmentPanel` White Balance controls and viewport WB picker | `RendererCore::fillUbuf()` -> shader white balance | XMP `crs:Temperature`, `crs:Tint` | White Balance |
| `saturation`, `vibrance` | `AdjustmentPanel` Colour sliders | `RendererCore::fillUbuf()` -> shader colour stage | XMP `crs:Saturation`, `crs:Vibrance` | Colour |
| `curveLuma`, `curveR`, `curveG`, `curveB` | `ToneCurveWidget` in `AdjustmentPanel` | `ImageViewport::ensureCurveLut()` -> curve LUT sampled by shader | XMP tone-curve sequences | Tone Curve |
| `hslHue`, `hslSat`, `hslLum` | `AdjustmentPanel` HSL / Color Mix pages | `RendererCore::fillUbuf()` -> shader HSL stage | XMP HSL adjustment attributes | HSL |
| `sharpening` | `AdjustmentPanel` Detail slider | Export workflow output sharpening | XMP `crs:Sharpness` | Detail |
| `rotation`, `cropRect`, `cropConstrained` | `AdjustmentPanel` Rotation slider and viewport Crop/Straighten tools | Vertex shader geometry; crop also sizes export/readbacks | XMP straighten/crop attributes | Geometry |
| `localAdjustments` | `LocalAdjustmentPanel` plus viewport mask tool | `RendererCore::fillUbuf()` local mask arrays -> shader | arraw-native XMP sequence | intentionally not copied by Develop Groups |
| `spots` | `SpotRemovalPanel` plus viewport spot tool | `MainWindow` applies spots to decoded buffers before upload/export | arraw-native XMP sequence | intentionally not copied by Develop Groups |

The guard tests are intentionally behavioral:

- XMP round-trip tests fail if persistence drops a field.
- Develop Group tests fail if any global field is left out of copy/preset flow.
- Golden image tests cover shader-visible tone, white balance, colour, HSL,
  tone curve, crop/rotation, local adjustments, and clipping behavior.
- Spot tests cover the pre-shader spot stage and its XMP round-trip.
