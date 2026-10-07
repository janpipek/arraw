#pragma once

#include <ColorEncoding.h>
#include <DevelopSettings.h>

namespace arraw {

/// @brief Everything that says how one photograph is developed.
///
/// Global settings now, per-image edits such as masks and spots later.
/// ::arraw::DevelopSettings is the global, descriptor-described part: what
/// presets, the JSON document and copy-paste between photographs carry. Edits
/// that belong to one image alone will be siblings of `settings`, as lists with
/// stable ids whose large payloads live in immutable shared storage so that
/// copying a state stays cheap. Culling marks are not part of it (see
/// ::arraw::PhotoMarks).
struct DevelopState {
    /// @brief Global photographic settings, the part presets and JSON carry.
    DevelopSettings settings{};

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
/// @param state State to check.
/// @throws std::invalid_argument as ::arraw::validate(const DevelopSettings&).
void validate(const DevelopState& state);

} // namespace arraw
