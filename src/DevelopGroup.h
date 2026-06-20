#pragma once

#include "ImagePipeline.h"

#include <bitset>

#include <QString>

// A Develop Group is one selectable unit in the Copy Settings / Develop Preset
// checklist — the granularity at which develop settings travel between photos
// (Milestone 8). The seven groups exhaustively partition every *global* field of
// GlobalAdjustment; Local Adjustments are deliberately not a group (masks pinned
// to one photo's framing rarely transfer). See docs/adr/0023 and CONTEXT.md.
enum class DevelopGroup {
    WhiteBalance, // temperature (Kelvin) + tint
    Tone,         // exposure, contrast, highlights, shadows, whites, blacks
    ToneCurve,    // Luma + R/G/B curves
    Colour,       // saturation, vibrance
    Hsl,          // the 8-band hue/sat/lum mix
    Detail,       // sharpening
    Geometry,     // rotation + crop rect + aspect-lock flag, as one unit
    Count_,       // sentinel — keep last
};

inline constexpr int kDevelopGroupCount = static_cast<int>(DevelopGroup::Count_);

// A set of groups. operator& gives the paste "narrow only" bound for free.
using GroupSelection = std::bitset<kDevelopGroupCount>;

inline GroupSelection allGroups() {
    GroupSelection s;
    s.set();
    return s;
}

inline bool hasGroup(GroupSelection s, DevelopGroup g) {
    return s.test(static_cast<size_t>(g));
}

// The copy/paste & preset checklist's initial selection: every group except
// Geometry, which is per-image (orientation/rotation/crop) and rarely transfers,
// so it ships unchecked and is opt-in (CONTEXT.md, docs/adr/0025).
inline GroupSelection defaultCopySelection() {
    GroupSelection s = allGroups();
    s.reset(static_cast<size_t>(DevelopGroup::Geometry));
    return s;
}

// Returns `target` with each selected group's fields overwritten by `source`'s
// (replace-wholesale: a selected group whose source is at default resets that
// group on the target). Unselected groups, and the localAdjustments list, are
// always taken from `target` unchanged. This is the single pure function behind
// copy/paste and preset apply (docs/adr/0023, [[spot-for-algorithms]]).
GlobalAdjustment applyGroups(
    const GlobalAdjustment& target, const GlobalAdjustment& source, GroupSelection selection);

// Stable machine key for a group — used as the JSON object key (docs/adr/0023)
// and as the checklist checkbox objectName. Never localised; never reordered.
const char* developGroupKey(DevelopGroup g);

// Human-readable label for a group, shown in the Copy/Paste and preset
// checklists. Localisable.
QString developGroupLabel(DevelopGroup g);
