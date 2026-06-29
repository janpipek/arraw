#pragma once

#include "develop/BrushMask.h"
#include "develop/FieldSpec.h"

#include <QPointF>
#include <QString>

#include <variant>
#include <vector>

// A graduated ("Linear") mask: the weight ramps from 0 at p0 to 1 at p1 along
// the line between them. Endpoints are normalised coordinates in the
// cropped/rotated display frame (docs/adr/0007), so recropping does not slide
// the mask. Weight is evaluated in aspect-corrected space so a diagonally drawn
// gradient stays perpendicular to its line on screen (see docs/adr/0010).
struct LinearMask {
    QPointF p0; // weight 0
    QPointF p1; // weight 1
    bool operator==(const LinearMask&) const = default;
};

// A Radial (oval) mask. Geometry is normalised display-frame coords; the radii
// and angle live in aspect-corrected space so the oval looks as drawn on screen.
// Weight is 1 inside, ramping to 0 at the boundary over `feather` (smoothstep);
// `invert` flips inside/outside (docs/adr/0010).
struct RadialMask {
    QPointF center{0.5, 0.5};
    double radiusX = 0.25;
    double radiusY = 0.25;
    double angle = 0.0;   // degrees
    double feather = 0.5; // 0..1 falloff band
    bool invert = false;
    bool operator==(const RadialMask&) const = default;
};

// The kind of mask. Each alternative is a small value struct with its own
// geometry; a new mask type is an additive new arm. The GPU flattens whichever
// arm into a type-tagged uniform slot (docs/adr/0010), and for the painted
// BrushMask also samples its raster at vUV (docs/adr/0047). LinearMask stays
// first so a default-constructed Mask is Linear.
using Mask = std::variant<LinearMask, RadialMask, BrushMask>;

// The dodge/burn + colour-grade scalar subset shared by global and local develop
// edits (docs/adr/0010). Zero = no change for every field. `exposure` is in EV;
// the rest are on the internal -100..100 scale.
struct SharedAdjustment {
    float exposure = 0.0f;
    float contrast = 0.0f;
    float highlights = 0.0f;
    float shadows = 0.0f;
    float whites = 0.0f;
    float blacks = 0.0f;
    float tint = 0.0f;
    float saturation = 0.0f;
    float vibrance = 0.0f;
    bool operator==(const SharedAdjustment&) const = default;
};

// A Local Adjustment: a Mask (where) plus the shared delta subset (what), applied
// to one region of one image. `temperature` here is a relative -100..100 shift,
// NOT absolute Kelvin like the global temperature (docs/adr/0010).
struct LocalAdjustment : SharedAdjustment {
    Mask mask;
    float temperature = 0.0f; // relative -100..100
    bool operator==(const LocalAdjustment&) const = default;
};

// Mask weight in [0,1] for a pixel at normalised display-frame UV `uv`.
// `aspect` = displayWidth / displayHeight, applied so the geometry is isotropic.
float maskWeight(const LinearMask& m, QPointF uv, float aspect);

// The draggable handles of a Linear mask: the two endpoints plus the derived
// midpoint (drag to translate the whole mask). Orientation and spread are
// implicit in the endpoints, so there is no separate rotation or feather handle.
enum class LinearHandle { None, P0, P1, Center };

// Which handle of `m` is under `cursor`, or None if none is within `pickRadius`.
// Distances use the same aspect-corrected space as maskWeight(), so the pick
// circle is round on screen.
LinearHandle nearestHandle(const LinearMask& m, QPointF cursor, float aspect, double pickRadius);

// Radial (oval) mask weight in [0,1] for a pixel at normalised display-frame UV
// `uv`. Aspect-corrected so the oval is round-on-screen when radiusX == radiusY.
float radialMaskWeight(const RadialMask& m, QPointF uv, float aspect);

// Draggable handles of a Radial mask: centre (move) and the +X / +Y axis ends
// (resize; the X handle also sets the angle from the centre→handle direction).
enum class RadialHandle { None, Center, RadiusX, RadiusY };

// UV position of a Radial handle, for drawing and hit-testing (aspect-corrected).
QPointF radialHandlePos(const RadialMask& m, RadialHandle h, float aspect);

// Move a Radial handle to `to` (normalised UV): Centre translates; RadiusX sets
// radiusX + angle from the centre→cursor vector; RadiusY sets radiusY along the
// perpendicular axis. `aspect` keeps the geometry isotropic.
RadialMask moveRadialHandle(RadialMask m, RadialHandle h, QPointF to, float aspect);

// Reposition a handle of `m` so it lands on `to` (normalised display coords).
// P0/P1 move that endpoint; Center translates both points (preserving
// orientation and spread). None returns the mask unchanged. No clamping.
LinearMask moveHandle(LinearMask m, LinearHandle h, QPointF to);

// One editable delta row of a Local Adjustment: its label, slider number-handling
// (FieldSpec), and the field it drives. The single source of truth for the Masks
// panel's delta sliders and for History labelling ([[spot-for-algorithms]]).
struct LocalDeltaField {
    const char* label; // shown in the Masks panel and the History list
    FieldSpec spec;
    float LocalAdjustment::* member;
};

// The delta rows in panel order (Exposure first, then the ±100 tone/colour
// shifts plus the relative temperature).
const std::vector<LocalDeltaField>& localDeltaFields();

// The display name of the mask at `index`, matching the Masks panel list
// ("Linear 2", "Radial 1") — the per-type ordinal in list order.
QString maskDisplayName(const std::vector<LocalAdjustment>& list, int index);

// One human label for a local-adjustment edit, derived from what changed between
// two mask lists — descriptive, so each step is distinct in the History list:
//   • a mask added            → "Add Linear Mask" / "Add Radial Mask"
//   • a mask removed          → "Delete Linear Mask" / "Delete Radial Mask"
//   • one mask's geometry moved → "Linear 2 Geometry"
//   • one delta changed       → "Linear 2 — Exposure +0.50 EV"
//   • anything else / nothing → "Adjust Local"
QString localChangeLabel(
    const std::vector<LocalAdjustment>& before, const std::vector<LocalAdjustment>& after);
