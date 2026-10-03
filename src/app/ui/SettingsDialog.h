#pragma once

#include "AppSettings.h"

#include <QDialog>

#include <vector>

class QComboBox;
class QWidget;

namespace arraw::app {

/// @brief Desktop preferences dialog.
class SettingsDialog : public QDialog {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(SettingsDialog)
public:
    /// @brief Builds the dialog from the saved preferences.
    explicit SettingsDialog(const AppSettings& initial, QWidget* parent = nullptr);

    /// @brief Returns the preferences currently selected in the dialog.
    [[nodiscard]] AppSettings settings() const;

private:
    QComboBox* deviceBox_ = nullptr;
    /// @brief Preferences corresponding to the device combo's rows.
    std::vector<AppSettings> choices_;
};

} // namespace arraw::app
