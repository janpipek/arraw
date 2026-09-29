#pragma once

#include <Photo.h>

#include <utility>

namespace arraw {

/// @brief One photograph being edited: the document as it stands now.
///
/// A session exists only while a photograph is open, so there is no empty
/// session: "nothing open" is a caller holding no session, not a session
/// holding nothing. It owns no pixels; decoding and rendering belong to
/// whoever displays or exports the photograph (ADR 001).
///
/// It is where edits happen, as opposed to ::arraw::Photo, which is a value
/// that edits produce. History and unsaved-change tracking will live here.
class EditSession {
public:
    /// @brief Starts editing a photograph.
    /// @param photo Document to edit, usually from ::arraw::openPhoto.
    explicit EditSession(Photo photo) : photo_(std::move(photo)) {}

    /// @brief Current state of the document being edited.
    [[nodiscard]] const Photo& photo() const noexcept {
        return photo_;
    }

    /// @brief Replaces how the photograph is developed.
    /// @param settings Settings the document carries from now on.
    void setSettings(DevelopSettings settings) {
        photo_ = photo_.with(settings);
    }

private:
    Photo photo_;
};
} // namespace arraw