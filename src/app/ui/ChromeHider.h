#pragma once

#include <vector>

class QWidget;

namespace arraw::app {

/// Set of widgets hidden together and restored to the visibility each had before.
///
/// The snapshot and restore behind Hide Panels (docs/adr/047).
class ChromeHider {
public:
    /// @brief Makes a hider over widgets it does not own.
    /// @param widgets Widgets to hide; each must outlive the hider.
    explicit ChromeHider(const std::vector<QWidget*>& widgets);

    /// @brief Notes which widgets are hidden already, then hides them all; does nothing when
    /// hidden.
    void hide();

    /// @brief Shows again the widgets that were shown at hide(); does nothing when not hidden.
    void restore();

    /// @brief Tells whether the widgets are hidden by this hider.
    /// @return True between hide() and restore().
    [[nodiscard]] bool hidden() const {
        return hidden_;
    }

private:
    /// A widget with the visibility the last hide() found.
    struct Entry {
        QWidget* widget;
        bool wasHidden = false; ///< Valid while hidden_.
    };

    std::vector<Entry> entries_;
    bool hidden_ = false;
};

} // namespace arraw::app
