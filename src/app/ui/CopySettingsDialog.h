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
    /// @param checked Sections to start checked; those not in ::arraw::copyableSections are
    /// ignored.
    /// @param parent Owning widget.
    explicit CopySettingsDialog(std::span<const CopySection> checked, QWidget* parent = nullptr);

    /// @brief Returns the sections currently checked.
    /// @return The sections, in enumeration order.
    [[nodiscard]] std::vector<CopySection> sections() const;

private:
    /// @brief Enables OK only while a section is checked.
    void updateAccept();

    /// @brief Checks or unchecks every box.
    void setAll(bool checked);

    std::vector<QCheckBox*> boxes_;
    QDialogButtonBox* buttons_ = nullptr;
};

} // namespace arraw::app
