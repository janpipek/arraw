#pragma once

#include <EditSession.h>
#include <Photo.h>

#include <QMainWindow>

#include <optional>

class QLabel;

namespace arraw::app {

/// @brief Top-level window of the desktop application.
class MainWindow : public QMainWindow {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(MainWindow)
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    /// @brief Builds the menu bar and the actions it offers.
    void buildMenu();

    /// @brief Builds the scrollable area the photograph is shown in.
    void buildImageView();

    /// @brief Asks the user for a photograph and opens it.
    void openFileWithDialog();

    /// @brief Renders a photograph and makes it the one being edited.
    ///
    /// Commits only once rendering has succeeded, so a failure leaves the
    /// window showing the previous photograph, session and pixels together.
    /// @param photo Photograph to show.
    /// @throws std::exception if the photograph cannot be decoded or rendered.
    void showPhoto(Photo photo);

    QLabel* imageView_ = nullptr;

    std::optional<EditSession> editSession_;
};

} // namespace arraw::app
