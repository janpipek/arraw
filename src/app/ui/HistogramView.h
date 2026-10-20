#pragma once

#include "HistogramPresentation.h"

#include <OutputHistogram.h>

#include <QRectF>
#include <QSize>
#include <QWidget>

#include <optional>

class QPaintEvent;

namespace arraw::app {

/// @brief Painted plot of an output histogram (ADR 055, section 7).
///
/// Draws the 254 interior bins as filled areas on an always dark ground: in RGB the three
/// channels composed additively, so overlaps show cyan, magenta, yellow and white; in luma one
/// area in the text colour. The height is a soft cap (::arraw::app::histogramScale), and a
/// one-pixel line marks where taller bins are cut. Bins 0 and 255 are not part of the area:
/// each is a marker column in a gutter beside the plot, at its height on the same scale.
///
/// The view paints into an image of its own and blits it, so that the additive blending does not
/// depend on the platform's backing store. It takes no focus and no input.
class HistogramView : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(HistogramView)
public:
    /// @brief Builds an empty view showing RGB.
    /// @param parent Owning widget.
    explicit HistogramView(QWidget* parent = nullptr);

    /// @brief Shows counts, or nothing.
    /// @param histogram Counts to draw; nothing clears the plot.
    void setHistogram(std::optional<OutputHistogram> histogram);

    /// @brief Tells whether counts are shown.
    [[nodiscard]] bool hasHistogram() const noexcept {
        return histogram_.has_value();
    }

    /// @brief Gives the counts shown.
    [[nodiscard]] const std::optional<OutputHistogram>& histogram() const noexcept {
        return histogram_;
    }

    /// @brief Chooses the channels drawn; a view change.
    /// @param channels Channels to draw.
    void setChannels(HistogramChannels channels);

    /// @brief Gives the channels drawn.
    [[nodiscard]] HistogramChannels channels() const noexcept {
        return channels_;
    }

    /// @brief Gives the rectangle the interior bins are drawn in, in widget pixels.
    [[nodiscard]] QRectF plotRect() const;

    /// @brief Gives the gutter beside the plot that holds the marker of bin 0 (left) or 255
    /// (right), in widget pixels.
    /// @param highlights Whether the marker of bin 255 is meant; else bin 0's.
    [[nodiscard]] QRectF markerRect(bool highlights) const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    /// @brief Words the state for assistive technology.
    void updateDescription();

    std::optional<OutputHistogram> histogram_;
    HistogramChannels channels_ = HistogramChannels::Rgb;
};

} // namespace arraw::app
