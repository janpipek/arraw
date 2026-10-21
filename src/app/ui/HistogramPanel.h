#pragma once

#include "HistogramPresentation.h"

#include <OutputHistogram.h>

#include <QColor>
#include <QRgb>
#include <QWidget>

#include <optional>

class QLabel;
class QToolButton;

namespace arraw::app {

class HistogramView;

/// @brief Histogram, clipping triangles, channel switch and pixel readout (ADR 055, section 7).
///
/// The body of the develop dock's `histogram` section; it does not collapse itself, and the
/// section that holds it remembers whether it is open. It holds:
/// - a HistogramView, with a triangle at each top corner of the plot: shadows on the left,
///   highlights on the right. A triangle is an outline while nothing clips and filled, in the
///   colour of the channels that clip, while something does. A click toggles painting that kind
///   of clipping on the picture;
/// - the RGB and Luma switch, remembered in QSettings under `histogram/channels`;
/// - a line that reads the colour under the pointer, which keeps its height.
///
/// The buttons take the focus by Tab only, never by a click (ADR 040). The panel only reports
/// the toggles; it paints nothing on the picture.
class HistogramPanel : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(HistogramPanel)
public:
    /// @brief Builds an empty panel, showing the channels the settings remember (RGB at first).
    /// @param parent Owning widget.
    explicit HistogramPanel(QWidget* parent = nullptr);

    /// @brief Shows counts; lights the triangles that have clipped pixels and enables them.
    /// @param histogram Counts to show.
    void showHistogram(const OutputHistogram& histogram);

    /// @brief Shows no counts; unlights and disables the triangles.
    void clear();

    /// @brief Tells whether counts are shown.
    [[nodiscard]] bool hasHistogram() const noexcept;

    /// @brief Shows the colour under the pointer in the readout.
    /// @param colour Colour of the pixel; empty off the picture, which shows dashes.
    void setPointerColour(std::optional<QRgb> colour);

    /// @brief Sets which kinds of clipping are painted on the picture, without emitting
    /// clippingToggled().
    /// @param shadows Whether shadow-clipped pixels are painted.
    /// @param highlights Whether highlight-clipped pixels are painted.
    void setClippingShown(bool shadows, bool highlights);

    /// @brief Tells whether a kind of clipping is set to be painted.
    [[nodiscard]] bool clippingShown(ClipKind kind) const;

    /// @brief Gives the colour a triangle is filled with, or nothing while it is unlit.
    [[nodiscard]] std::optional<QColor> triangleColour(ClipKind kind) const;

    /// @brief Gives the channels drawn.
    [[nodiscard]] HistogramChannels channels() const noexcept;

    /// @brief Gives the text of the readout.
    [[nodiscard]] QString readout() const;

    /// @brief Gives the plot.
    [[nodiscard]] HistogramView* view() const noexcept {
        return view_;
    }

signals:
    /// @brief The user clicked a triangle.
    /// @param shadows Whether shadow-clipped pixels are now to be painted.
    /// @param highlights Whether highlight-clipped pixels are now to be painted.
    void clippingToggled(bool shadows, bool highlights);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// @brief Puts the triangles at the plot's top corners.
    void placeTriangles();

    /// @brief Words a triangle's tooltip from the counts, or says there are none.
    void updateTriangles();

    HistogramView* view_ = nullptr;
    QToolButton* shadows_ = nullptr;
    QToolButton* highlights_ = nullptr;
    QToolButton* rgb_ = nullptr;
    QToolButton* luma_ = nullptr;
    QLabel* readout_ = nullptr;
};

} // namespace arraw::app
