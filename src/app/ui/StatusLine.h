#pragma once

#include <QString>
#include <QTimer>
#include <QWidget>

class QLabel;
class QToolButton;

namespace arraw::app {

class RenderProgressPie;

/// @brief Status-bar line of the render pie, a message, the device and the zoom control.
///
/// Replaces QStatusBar's own message mechanism, which hides every ordinary widget while a message
/// shows: here the pie sits at the left and stays visible beside the message (ADR 042).
/// Status tips must not be used on this window's widgets; MainWindow sends them here.
class StatusLine : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(StatusLine)
public:
    /// @brief Builds the line.
    /// @param zoomButton Zoom control to put at the right end; the line takes it over.
    /// @param parent Owner of the widget.
    explicit StatusLine(QToolButton* zoomButton, QWidget* parent = nullptr);

    /// @brief Render pie at the left end.
    [[nodiscard]] RenderProgressPie* pie() const noexcept {
        return pie_;
    }

    /// @brief Label that shows the message.
    [[nodiscard]] QLabel* messageLabel() const noexcept {
        return message_;
    }

    /// @brief Label that names the preview device.
    [[nodiscard]] QLabel* deviceLabel() const noexcept {
        return device_;
    }

    /// @brief Message being shown, in full; empty when none.
    [[nodiscard]] const QString& message() const noexcept {
        return text_;
    }

    /// @brief Shows a message, replacing any shown and restarting the timeout.
    /// @param text What to say.
    /// @param timeoutMs Milliseconds until it clears itself; 0 keeps it until replaced or cleared.
    /// @param tip Tooltip for details that do not fit; empty to show the full text when elided.
    void showMessage(const QString& text, int timeoutMs = 0, const QString& tip = {});

    /// @brief Clears the message, its tooltip and its timeout.
    void clearMessage();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// @brief Fits the shown text to the message label and sets its tooltip.
    void elide();

    RenderProgressPie* pie_ = nullptr;
    QLabel* message_ = nullptr;
    QLabel* device_ = nullptr;
    QTimer timer_;
    /// Message in full.
    QString text_;
    /// Tooltip given with the message.
    QString tip_;
};

} // namespace arraw::app
