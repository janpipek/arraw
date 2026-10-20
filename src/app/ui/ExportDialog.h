#pragma once

#include <Diagnostics.h>
#include <ExportPresets.h>
#include <ExportSettings.h>
#include <ImageBuffer.h>

#include <QDialog>
#include <QString>

#include <functional>
#include <optional>
#include <string>

class QAction;
class QCheckBox;
class QComboBox;
class QFormLayout;
class QGroupBox;
class QPushButton;
class QSlider;
class QSpinBox;
class QToolButton;
class QVBoxLayout;
class QWidget;

namespace arraw::app {

/// @brief Dialog that gathers the settings of an export.
///
/// Only gathers: what the settings mean, and the file they go to, are decided
/// elsewhere (see ExportSettings). With a preset store it also shows a picker of the named
/// presets: choosing one sets every control, and Save As, Update, Rename and Delete change the
/// presets file at once (ADR 054).
class ExportDialog : public QDialog {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(ExportDialog)
public:
    /// @brief Builds the dialog showing settings.
    /// @param initial Settings to start from; a resize box that is no preset long edge is
    ///        the custom size.
    /// @param frame Size of the developed frame, which a custom width and height start at
    ///        unless initial has its own.
    /// @param store Presets to offer; nothing hides the picker. Must outlive the dialog.
    /// @param rememberedPreset Name of the preset to show as chosen; one that is gone shows
    ///        "No preset". The controls show initial either way, and the preset is marked
    ///        modified when they differ from it.
    /// @param parent Owning widget.
    ExportDialog(const ExportSettings& initial, ImageSize frame, ExportPresetStore* store = nullptr,
                 std::optional<std::string> rememberedPreset = std::nullopt,
                 QWidget* parent = nullptr);

    /// @brief The questions the preset picker asks, which a test may answer without a dialog.
    ///
    /// Each member left empty is the real dialog.
    struct Prompts {
        /// Asks for a preset name; receives the title, the label and the text to start with.
        /// Gives nothing when cancelled.
        std::function<std::optional<QString>(const QString&, const QString&, const QString&)> name;
        /// Asks a yes or no question; receives the title and the text.
        std::function<bool(const QString&, const QString&)> confirm;
        /// Reports a problem; receives the title and the text.
        std::function<void(const QString&, const QString&)> warn;
    };

    /// @brief Replaces the questions of the preset picker; for tests.
    /// @param prompts Answers; a member left empty keeps the real dialog.
    void setPrompts(Prompts prompts) {
        prompts_ = std::move(prompts);
    }

    /// @brief Sends the problems found when reading the presets file to a log.
    /// @param log The log; it must outlive the dialog. Nothing is reported by default. Reads the
    /// presets file again.
    void setDiagnostics(DiagnosticLog* log);

    /// @brief Gives the settings as the dialog shows them now.
    [[nodiscard]] ExportSettings settings() const;

    /// @brief Gives the name of the preset chosen in the picker, modified or not.
    /// @return The name, or nothing for "No preset", a preset hidden by a built-in, or without
    /// a store.
    [[nodiscard]] std::optional<std::string> chosenPreset() const;

private:
    /// @brief Enables the controls that apply to the chosen format and size choice.
    void updateEnabled();

    /// @brief Builds the preset row at the top.
    void buildPresetRow(QVBoxLayout* root);

    /// @brief Reads the catalog again and fills the picker, choosing a preset by name.
    /// @param name Preset to choose; nothing chooses "No preset".
    void reloadPresets(const std::optional<std::string>& name);

    /// @brief Finds a usable preset of the catalog by name.
    /// @param name Name to look for.
    /// @param shadowed Whether to look among the presets hidden by a built-in instead.
    [[nodiscard]] const ExportPreset* findPreset(const std::string& name, bool shadowed) const;

    /// @brief Gives the sentence that says why a hidden preset cannot be used.
    [[nodiscard]] QString hiddenReason() const;

    /// @brief Sets every control to settings.
    void apply(const ExportSettings& settings);

    /// @brief Marks the picker's item modified or not, and updates the buttons.
    void refreshPresetState();

    /// @brief Tells whether the chosen preset's settings differ from the controls'.
    [[nodiscard]] bool modified() const;

    /// @brief Gives the reason that writing the presets file is not possible.
    /// @return The reason, or an empty string when it is possible.
    [[nodiscard]] QString writeBlock() const;

    /// @brief Saves the controls under a name asked for.
    void saveAsPreset();

    /// @brief Saves the controls over the chosen preset.
    void updatePreset();

    /// @brief Gives the chosen preset a name asked for.
    void renamePreset();

    /// @brief Removes the chosen preset after confirmation.
    void deletePreset();

    /// @brief Opens the folder of the presets file.
    void showPresetsFile();

    /// @brief Moves an unreadable presets file aside after confirmation.
    void setAsidePresets();

    /// @brief Asks for a name, through the prompts or the real dialog.
    [[nodiscard]] std::optional<QString> askName(const QString& title, const QString& label,
                                                 const QString& initial);

    /// @brief Asks a yes or no question, through the prompts or the real dialog.
    [[nodiscard]] bool confirm(const QString& title, const QString& text);

    /// @brief Reports a problem, through the prompts or the real dialog.
    void warn(const QString& title, const QString& text);

    /// @brief Runs a change to the presets file, reporting what goes wrong.
    /// @return Whether it was made.
    bool change(const QString& title, const std::function<void()>& action);

    QComboBox* formatBox_ = nullptr;
    QComboBox* profileBox_ = nullptr;
    QCheckBox* sixteenBitCheck_ = nullptr;
    QComboBox* sizeBox_ = nullptr;
    QFormLayout* sizeForm_ = nullptr;
    QSpinBox* widthSpin_ = nullptr;
    QSpinBox* heightSpin_ = nullptr;
    QCheckBox* enlargeCheck_ = nullptr;
    QGroupBox* qualityGroup_ = nullptr;
    QSlider* qualitySlider_ = nullptr;
    QSpinBox* qualitySpin_ = nullptr;
    QSlider* sharpenSlider_ = nullptr;
    QSpinBox* sharpenSpin_ = nullptr;
    QCheckBox* captureCheck_ = nullptr;
    QCheckBox* locationCheck_ = nullptr;
    QCheckBox* descriptiveCheck_ = nullptr;

    ExportPresetStore* store_ = nullptr; ///< Presets file; null without a picker.
    ExportPresetCatalog catalog_;        ///< What the file held when last read.
    std::optional<ExportPreset> chosen_; ///< The preset the picker shows as chosen.
    QComboBox* presetBox_ = nullptr;
    QPushButton* okButton_ = nullptr;
    DiagnosticLog* diagnostics_ = nullptr; ///< Where the file's problems go, if anywhere.
    QPushButton* saveAsButton_ = nullptr;
    QToolButton* presetMenuButton_ = nullptr;
    QAction* updateAction_ = nullptr;
    QAction* renameAction_ = nullptr;
    QAction* deleteAction_ = nullptr;
    QAction* showFileAction_ = nullptr;
    QAction* setAsideAction_ = nullptr;
    Prompts prompts_;
};

} // namespace arraw::app
