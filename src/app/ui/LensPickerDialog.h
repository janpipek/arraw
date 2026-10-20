#pragma once

#include <LensDatabase.h>
#include <LensMatch.h>

#include <QDialog>

#include <optional>
#include <string>
#include <vector>

class QDialogButtonBox;
class QLineEdit;
class QListWidget;

namespace arraw::app {

/// @brief Dialog choosing the lens a photograph's corrections are for (ADR 050, section 6).
///
/// Lists the lenses of the database that fit the camera's mount, searchable by words of the
/// name, with the lenses that fitted a photograph equally well first.
class LensPickerDialog : public QDialog {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(LensPickerDialog)
public:
    /// @brief Most lenses the list shows at once; a search narrows it.
    static constexpr std::size_t maximumListed = 400;

    /// @brief Builds the dialog.
    /// @param database Database to list; must outlive the dialog.
    /// @param mount Mount of the photograph's camera, or nothing for every lens.
    /// @param candidates Lenses that fitted equally well, listed first while nothing is searched.
    /// @param parent Owning widget.
    LensPickerDialog(const LensDatabase& database, std::optional<std::string> mount,
                     std::vector<LensIdentity> candidates, QWidget* parent = nullptr);

    /// @brief Gives the lens selected in the list.
    /// @return The lens, or nothing when none is selected.
    [[nodiscard]] std::optional<LensIdentity> chosen() const;

    /// @brief Fills the list for a search.
    /// @param search Words that must all occur in the lens's name; empty for every lens.
    void showMatches(const QString& search);

private:
    const LensDatabase& database_;
    std::optional<std::string> mount_;
    std::vector<LensIdentity> candidates_;
    QLineEdit* search_ = nullptr;
    QListWidget* list_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
};

} // namespace arraw::app
