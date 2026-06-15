#pragma once

#include <QPointF>

#include <variant>

// A graduated ("Linear") mask: the weight ramps from 0 at p0 to 1 at p1 along
// the line between them. Endpoints are normalised coordinates in the
// cropped/rotated display frame (docs/adr/0007), so recropping does not slide
// the mask. Weight is evaluated in aspect-corrected space so a diagonally drawn
// gradient stays perpendicular to its line on screen (see docs/adr/0010).
struct LinearMask {
    QPointF p0;  // weight 0
    QPointF p1;  // weight 1
    bool operator==(const LinearMask&) const = default;
};

// The kind of parametric mask. Each alternative is a small value struct with its
// own geometry; a new mask type is an additive new arm (Radial next). The GPU
// flattens whichever arm into a type-tagged uniform slot (docs/adr/0010).
using Mask = std::variant<LinearMask>;

// The dodge/burn + colour-grade scalar subset shared by global and local develop
// edits (docs/adr/0010). Zero = no change for every field. `exposure` is in EV;
// the rest are on the internal -100..100 scale.
struct SharedAdjustment {
    float exposure   = 0.0f;
    float contrast   = 0.0f;
    float highlights = 0.0f;
    float shadows    = 0.0f;
    float whites     = 0.0f;
    float blacks     = 0.0f;
    float tint       = 0.0f;
    float saturation = 0.0f;
    float vibrance   = 0.0f;
    bool operator==(const SharedAdjustment&) const = default;
};

// A Local Adjustment: a Mask (where) plus the shared delta subset (what), applied
// to one region of one image. `temperature` here is a relative -100..100 shift,
// NOT absolute Kelvin like the global temperature (docs/adr/0010).
struct LocalAdjustment : SharedAdjustment {
    Mask  mask;
    float temperature = 0.0f;  // relative -100..100
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
LinearHandle nearestHandle(const LinearMask& m, QPointF cursor, float aspect,
                           double pickRadius);

// Reposition a handle of `m` so it lands on `to` (normalised display coords).
// P0/P1 move that endpoint; Center translates both points (preserving
// orientation and spread). None returns the mask unchanged. No clamping.
LinearMask moveHandle(LinearMask m, LinearHandle h, QPointF to);
