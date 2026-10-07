#pragma once

#include <Edits.h>

#include <QDialog>

#include <span>
#include <vector>

class QCheckBox;
class QDialogButtonBox;

namespace arraw::app {

/// @brief Dialog choosing which sections of a photograph's settings to copy.
class CopySettingsDialog : public QDialog {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(CopySettingsDialog)
public:
    /// @brief Builds the dialog with the given sections checked.
    ///
    /// A section with nothing to copy from this photograph (White Balance from one that is not a
    /// RAW) is shown disabled and unchecked, and keeps its remembered choice for the next RAW.
    /// @param checked Sections to start checked; those not in ::arraw::copyableSections are
    /// ignored.
    /// @param fromRaw Whether the photograph copied from is a RAW.
    /// @param parent Owning widget.
    CopySettingsDialog(std::span<const CopySection> checked, bool fromRaw,
                       QWidget* parent = nullptr);

    /// @brief Returns the sections to copy: those checked and available.
    /// @return The sections, in enumeration order.
    [[nodiscard]] std::vector<CopySection> sections() const;

    /// @brief Returns the choice to remember: the sections checked, and the unavailable ones as
    /// they were given.
    /// @return The sections, in enumeration order.
    [[nodiscard]] std::vector<CopySection> remembered() const;

private:
    /// @brief Enables OK only while a section is checked.
    void updateAccept();

    /// @brief Checks or unchecks every box.
    void setAll(bool checked);

    std::vector<QCheckBox*> boxes_;
    /// Choice given for each box, kept for the disabled ones.
    std::vector<bool> given_;
    QDialogButtonBox* buttons_ = nullptr;
};

} // namespace arraw::app
