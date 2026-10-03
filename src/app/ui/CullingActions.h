#pragma once

#include <PhotoMarks.h>

#include <QObject>

#include <utility>
#include <vector>

class QAction;
class QEvent;
class QMainWindow;

namespace arraw::app {

class FilmStrip;

/// @brief The Image menu: rating, colour labels and stepping between shots.
///
/// Builds the actions with the keys of main's film strip (0 to 5, X, R Y G B P) and the
/// arrow keys, and routes them to a ::arraw::app::FilmStrip. The actions belong to the
/// window, so the keys work whichever widget has the focus, except while a text field
/// takes them for typing: the shortcut machinery asks the focused widget first, and a
/// spin box's line edit keeps plain digits and arrows. Ctrl+Left and Ctrl+Right are the
/// exception and always step, as the plan wants (a line edit would take them to move
/// by a word); that is why this object filters the application's shortcut-override events.
class CullingActions : public QObject {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(CullingActions)
public:
    /// @brief Adds the Image menu to a window's menu bar.
    /// @param window Window whose menu bar gets the menu, and whose shortcuts these are.
    /// @param strip Strip the actions act on.
    CullingActions(QMainWindow& window, FilmStrip& strip);

    /// @brief Stops filtering the application's events.
    ~CullingActions() override;

protected:
    /// @brief Lets Ctrl+Left and Ctrl+Right through to their actions while text is edited.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// @brief Ticks the items that describe the active shot and enables the ones that can act.
    void reflectActiveShot();

    QMainWindow& window_;
    FilmStrip& strip_;
    /// Rating actions with the rating each sets.
    std::vector<std::pair<QAction*, int>> ratingActions_;
    /// Label actions with the label each toggles.
    std::vector<std::pair<QAction*, ColorLabel>> labelActions_;
    std::vector<QAction*> otherActions_;
};

} // namespace arraw::app
