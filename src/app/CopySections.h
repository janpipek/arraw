#pragma once

#include <Edits.h>

#include <QSettings>
#include <QString>

#include <span>
#include <vector>

namespace arraw::app {

/// @brief Look held for pasting, with the sections its copy chose.
struct SettingsClipboard {
    /// @brief Settings taken from the copied photograph, and whether it was a RAW.
    Look look;

    /// @brief Sections chosen when copying, in enumeration order.
    std::vector<CopySection> sections;
};

/// @brief Gives the name a copy section is shown under, in the words of the develop panel's groups.
/// @param section Section to name.
/// @return The translated label.
[[nodiscard]] QString copySectionLabel(CopySection section);

/// @brief Reads the sections the copy dialog had checked last time.
///
/// A missing key gives ::arraw::defaultCopySections; an empty stored list is
/// kept as "none checked". Unknown names and sections that cannot be copied
/// are dropped.
/// @param store Settings to read.
/// @return The sections, in enumeration order, each once.
[[nodiscard]] std::vector<CopySection> restoreCopySections(QSettings& store);

/// @brief Stores the sections the copy dialog had checked, by their stable names.
/// @param sections Sections to remember.
/// @param store Settings to write.
void saveCopySections(std::span<const CopySection> sections, QSettings& store);

/// @brief Words the sections a paste could not carry.
/// @param skipped Sections ::arraw::AppliedLook::skipped lists.
/// @return A translated sentence, or an empty string when nothing was skipped.
[[nodiscard]] QString skippedMessage(std::span<const CopySection> skipped);

} // namespace arraw::app
