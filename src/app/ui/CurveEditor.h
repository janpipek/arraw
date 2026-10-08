#pragma once

#include "CurveEditing.h"

#include <CurveHistogram.h>
#include <ToneCurveSettings.h>

#include <QString>
#include <QTimer>
#include <QWidget>

#include <cstddef>
#include <optional>

class QEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QFocusEvent;

namespace arraw::app {

/// @brief Painted editor of the four tone curves, one channel at a time.
///
/// Draws a square plot of the perceptual coordinate (ADR 010): a grid, the
/// curve-input histogram behind (ADR 035), and the channel's curve as the
/// engine resolves it, with its control points on top.
///
/// Editing, every step keeping the curve well formed (ADR 033):
/// - a click on the empty plot adds a point there and starts dragging it;
/// - dragging a point moves it, never past a neighbour, an end only up and down;
/// - dragging a point out of the plot removes it, and bringing it back restores it;
/// - a double-click or right-click on a point, or Delete on the selected one, removes it;
/// - the arrow keys move the selected point, Page Up and Page Down select the
///   previous and next point, and Esc or Enter hand the focus back.
///
/// Takes the focus on a click as well as on Tab, unlike SettingSlider: it has
/// no spin box beside it, so it is the keyboard way to a point (ADR 036).
/// Describes the selected point in readout(), for a label under the plot.
///
/// Reports edits in the shape of the edit protocol (ADR 022), as
/// SettingSlider does: a drag is one edit, begun by the first change it makes
/// (a press alone is no edit); arrow-key moves that follow each other closely
/// are one edit, which ends once they pause, focus leaves, or
/// finishPendingEdit() is called; a removal and a reset are edits of their own.
class CurveEditor : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(CurveEditor)
public:
    /// @brief Builds the editor showing the identity curves on the luma channel.
    /// @param parent Owning widget.
    explicit CurveEditor(QWidget* parent = nullptr);

    /// @brief Shows curves without emitting any signal.
    ///
    /// The selection is kept while it still names a point.
    /// @param curves Curves to show; also the base of the next edit.
    void setCurves(const ToneCurveSettings& curves);

    /// @brief Gives the curves shown.
    [[nodiscard]] const ToneCurveSettings& curves() const noexcept {
        return curves_;
    }

    /// @brief Switches the channel edited; a view change, not an edit.
    ///
    /// Ends a pending edit and clears the selection.
    /// @param channel Channel to show and edit.
    void setChannel(CurveChannel channel);

    /// @brief Gives the channel edited.
    [[nodiscard]] CurveChannel channel() const noexcept {
        return channel_;
    }

    /// @brief Shows the curve-input histogram behind the curves, or none.
    /// @param histogram Counts to draw; nothing clears the plot behind the curves.
    void setHistogram(std::optional<CurveHistogram> histogram);

    /// @brief Tells whether a histogram is drawn.
    [[nodiscard]] bool hasHistogram() const noexcept {
        return histogram_.has_value();
    }

    /// @brief Gives the index of the selected point of the channel's curve, if any.
    [[nodiscard]] std::optional<std::size_t> selectedPoint() const noexcept {
        return selected_;
    }

    /// @brief Gives the selected point's input and output, as "In 0.25 → Out 0.31", or nothing.
    ///
    /// In the perceptual coordinate the plot shows, to two decimals; empty
    /// while no point is selected.
    [[nodiscard]] const QString& readout() const noexcept {
        return readout_;
    }

    /// @brief Resets the channel's curve to the identity, as one complete edit.
    ///
    /// Does nothing when it already is.
    void resetChannel();

    /// @brief Ends an edit of arrow-key moves that is still waiting to end.
    ///
    /// Emits editFinished() when such an edit is open; does nothing otherwise.
    void finishPendingEdit();

    /// @brief Gives the square the curve is drawn in, in widget pixels.
    [[nodiscard]] QRectF plotRect() const;

    /// @brief Maps a curve coordinate to widget pixels.
    [[nodiscard]] QPointF toWidget(CurvePoint point) const;

    /// @brief Maps widget pixels to a curve coordinate; may lie outside 0 to 1.
    [[nodiscard]] CurvePoint toCurve(QPointF position) const;

    [[nodiscard]] bool hasHeightForWidth() const override;
    [[nodiscard]] int heightForWidth(int width) const override;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

signals:
    /// @brief Announces that an edit begins.
    void editStarted();

    /// @brief Announces the curve an edit has reached.
    /// @param channel Channel whose curve changed.
    /// @param curve The channel's curve now; well formed.
    void curveEdited(arraw::app::CurveChannel channel, const arraw::ToneCurve& curve);

    /// @brief Announces that the edit is over.
    void editFinished();

    /// @brief Announces that the edit made by resetChannel() is over.
    ///
    /// Takes the place of editFinished() for that edit, so that a listener can tell a reset from
    /// any other edit.
    void resetFinished();

    /// @brief Asks for the keyboard focus to go back to the photograph.
    void focusReleased();

    /// @brief Announces that readout() changed.
    /// @param text The new readout; empty when no point is selected.
    void readoutChanged(const QString& text);

protected:
    /// @brief Claims the editor's keys ahead of the window's shortcuts while it has the focus.
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    /// @brief Gives the curve of the channel shown.
    [[nodiscard]] const ToneCurve& curve() const;

    /// @brief Finds the point under a widget position, within the grab radius.
    [[nodiscard]] std::optional<std::size_t> pointAt(QPointF position) const;

    /// @brief Replaces the channel's curve and reports it, if it changed.
    /// @return Whether it changed.
    bool apply(const ToneCurve& curve);

    /// @brief Removes a point as one complete edit, if it may be removed.
    void removeAsEdit(std::size_t index);

    /// @brief Moves the selected point by a step, as part of a pending edit.
    void nudge(float dx, float dy);

    /// @brief Schedules a repaint and brings readout() up to date, announcing a change.
    void refresh();

    /// @brief Ends the drag in progress, finishing its edit if it made one.
    void endDrag();

    ToneCurveSettings curves_;
    CurveChannel channel_ = CurveChannel::Luma;
    std::optional<CurveHistogram> histogram_;
    std::optional<std::size_t> selected_;

    /// Text of the selected point, as readout() gives it.
    QString readout_;

    /// Drag in progress, from the press that grabbed or added a point.
    std::optional<CurvePointDrag> drag_;

    /// Point the latest press added, which a double-click's second press must not remove.
    std::optional<std::size_t> addedByPress_;

    /// Whether the drag in progress has begun an edit.
    bool dragEditing_ = false;

    /// Whether an edit of arrow-key moves is open.
    bool pending_ = false;

    /// Single-shot timer that ends a pending edit once its changes pause.
    QTimer pendingTimer_;
};

} // namespace arraw::app
