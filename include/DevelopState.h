#pragma once

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

/// @brief Checks a state.
/// @param state State to check.
/// @throws std::invalid_argument as ::arraw::validate(const DevelopSettings&).
void validate(const DevelopState& state);

} // namespace arraw
