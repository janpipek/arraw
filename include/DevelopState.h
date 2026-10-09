#pragma once

#include <ColorEncoding.h>
#include <DevelopSettings.h>
#include <LocalAdjustments.h>

#include <vector>

namespace arraw {

/// @brief Everything that says how one photograph is developed.
///
/// Global settings, and the edits that belong to this photograph alone: its local adjustments
/// (masks) now, spots later. ::arraw::DevelopSettings is the global, descriptor-described part:
/// what presets, the JSON document and copy-paste between photographs carry. The per-image edits
/// are siblings of `settings`, as lists with stable ids whose large payloads (a brush's strokes,
/// later) live in immutable shared storage so that copying a state stays cheap. Culling marks are
/// not part of it (see ::arraw::PhotoMarks).
struct DevelopState {
    /// @brief Global photographic settings, the part presets and JSON carry.
    DevelopSettings settings{};

    /// @brief Masked adjustments of this photograph alone, at most
    /// ::arraw::maximumLocalAdjustments, in the order they sum in (ADR 044).
    ///
    /// Not part of a look: copy and paste, and presets, leave a target's list as it is.
    std::vector<LocalAdjustment> localAdjustments{};

    /// @brief Identity the next added adjustment takes; the counter starts at 1 and never goes
    /// down, so removing an adjustment does not free its id.
    LocalAdjustmentId nextLocalAdjustmentId{1};

    friend bool operator==(const DevelopState&, const DevelopState&) = default;
};

/// @brief Gives the state a photograph of some kind starts from, before anyone edits it.
///
/// The one place a photograph's defaults depend on what it is (ADR 039). A
/// default-constructed state is the neutral one, the descriptor table's: it
/// leaves the pixels as decoded. A RAW, whose encoding is its camera's own,
/// starts with colour noise reduction at
/// ::arraw::rawDefaultColorNoiseReduction instead, as in Lightroom, because
/// demosaiced sensor data always carries colour noise; anything else starts
/// neutral, as an encoded picture has been through someone's noise reduction
/// already.
///
/// What uses it: a photograph opened with no sidecar, or with one that records
/// no develop settings (::arraw::SidecarContents::state), a photograph made from
/// a file without a state (::arraw::Photo's two-argument constructor), a
/// setting reset to its default in the develop panel, and a development from a
/// buffer with no state in Python. A sidecar that records settings is taken as
/// it is: a key it leaves out keeps the neutral default, so sidecars written
/// before a setting existed render as they did.
/// @param encoding Encoding the photograph's decoded pixels are in
/// (::arraw::ImageMetadata::encoding).
/// @return The state to start from.
[[nodiscard]] DevelopState defaultStateFor(const ColorEncoding& encoding);

/// @brief Checks a state.
///
/// The settings by ::arraw::validate(const DevelopSettings&), then the local adjustments (ADR
/// 044): at most ::arraw::maximumLocalAdjustments; ids that are not zero, not repeated, and
/// below the counter; opacity and feather within 0 to 1; each delta within its row of
/// ::arraw::localAdjustmentDescriptors; handle positions within
/// ::arraw::minimumMaskPosition to ::arraw::maximumMaskPosition; radii within
/// ::arraw::minimumMaskExtent to ::arraw::maximumMaskRadius; an angle within [-180, 180); a
/// linear mask's ends at least ::arraw::minimumMaskExtent apart; every number finite; a name
/// that is valid UTF-8 without control characters or characters XML cannot hold.
/// @param state State to check.
/// @throws std::invalid_argument naming the first thing that is wrong.
void validate(const DevelopState& state);

} // namespace arraw
