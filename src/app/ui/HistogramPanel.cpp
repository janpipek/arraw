#include "HistogramPanel.h"

#include "HistogramView.h"
#include "ThemeColors.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPolygonF>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

namespace arraw::app {

namespace {

/// Side of a triangle button, in pixels.
constexpr int triangleSize = 18;
/// Distance of a triangle from the corner of the plot's frame.
constexpr int triangleInset = 2;

/// Settings key the channel switch is remembered under.
const QString channelsKey = QStringLiteral("histogram/channels");

/// @brief Checkable button drawn as a right triangle in a corner: an outline, or filled when lit.
class TriangleButton : public QToolButton {
public:
    /// @brief Builds the button.
    /// @param pointsRight Whether the right angle is at the top right; else at the top left.
    /// @param parent Owning widget.
    TriangleButton(bool pointsRight, QWidget* parent) : QToolButton(parent), right_(pointsRight) {
        setCheckable(true);
        setAutoRaise(true);
        setFixedSize(triangleSize, triangleSize);
        setFocusPolicy(Qt::TabFocus);
        setCursor(Qt::PointingHandCursor);
    }

    /// @brief Sets the fill, or none for an outline.
    void setLit(std::optional<QColor> colour) {
        if (colour != lit_) {
            lit_ = colour;
            update();
        }
    }

    /// @brief Gives the fill, or none for an outline.
    [[nodiscard]] const std::optional<QColor>& lit() const noexcept {
        return lit_;
    }

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal nearEdge = 2.5;
        const qreal farEdge = triangleSize - 2.5;
        const QPolygonF shape =
            right_ ? QPolygonF({{farEdge, nearEdge}, {nearEdge, nearEdge}, {farEdge, farEdge}})
                   : QPolygonF({{nearEdge, nearEdge}, {farEdge, nearEdge}, {nearEdge, farEdge}});
        const QPalette& colours = palette();
        if (isChecked() && isEnabled()) {
            // The clipped pixels are painted on the picture: a frame in the accent.
            painter.setPen(QPen(colours.color(QPalette::Highlight), 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(0.75, 0.75, triangleSize - 1.5, triangleSize - 1.5), 2,
                                    2);
        }
        // Fixed tones: the triangles sit on the plot's dark ground whatever the theme.
        const QColor outline = isEnabled() ? theme::text : theme::disabledText;
        painter.setPen(
            QPen(lit_ ? *lit_ : outline, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(lit_ ? QBrush(*lit_) : QBrush(Qt::NoBrush));
        painter.drawPolygon(shape);
        if (hasFocus()) {
            painter.setPen(QPen(colours.color(QPalette::Highlight), 1.0, Qt::DotLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(0.5, 0.5, triangleSize - 1.0, triangleSize - 1.0));
        }
    }

private:
    bool right_;
    std::optional<QColor> lit_;
};

/// @brief Makes a text button of the channel switch.
QToolButton* switchButton(const QString& text, const QString& name, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setObjectName(name);
    button->setText(text);
    button->setCheckable(true);
    button->setAutoExclusive(true);
    button->setAutoRaise(false);
    button->setFocusPolicy(Qt::TabFocus);
    button->setAccessibleName(text);
    return button;
}

TriangleButton* asTriangle(QToolButton* button) {
    return static_cast<TriangleButton*>(button);
}

} // namespace

HistogramPanel::HistogramPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("histogramPanel");

    view_ = new HistogramView(this);
    view_->installEventFilter(this);

    shadows_ = new TriangleButton(false, view_);
    shadows_->setObjectName("shadowsTriangle");
    shadows_->setAccessibleName(tr("Show clipped shadows"));
    highlights_ = new TriangleButton(true, view_);
    highlights_->setObjectName("highlightsTriangle");
    highlights_->setAccessibleName(tr("Show clipped highlights"));
    for (QToolButton* triangle : {shadows_, highlights_}) {
        connect(triangle, &QToolButton::clicked, this,
                [this] { emit clippingToggled(shadows_->isChecked(), highlights_->isChecked()); });
    }

    rgb_ = switchButton(tr("RGB"), "channelRgb", this);
    luma_ = switchButton(tr("Luma"), "channelLuma", this);
    const bool luma = QSettings().value(channelsKey).toString() == QStringLiteral("luma");
    (luma ? luma_ : rgb_)->setChecked(true);
    view_->setChannels(luma ? HistogramChannels::Luma : HistogramChannels::Rgb);
    connect(rgb_, &QToolButton::clicked, this, [this] {
        view_->setChannels(HistogramChannels::Rgb);
        QSettings().setValue(channelsKey, QStringLiteral("rgb"));
    });
    connect(luma_, &QToolButton::clicked, this, [this] {
        view_->setChannels(HistogramChannels::Luma);
        QSettings().setValue(channelsKey, QStringLiteral("luma"));
    });

    readout_ = new QLabel(this);
    readout_->setObjectName("histogramReadout");
    readout_->setAccessibleName(tr("Colour under the pointer"));
    readout_->setMinimumHeight(readout_->fontMetrics().height() + 2);
    // The text is not meant to be announced on every move.
    readout_->setTextInteractionFlags(Qt::NoTextInteraction);
    readout_->setText(readoutText(std::nullopt));

    auto* bottom = new QHBoxLayout;
    bottom->setContentsMargins(0, 0, 0, 0);
    bottom->addWidget(readout_, 1);
    bottom->addWidget(rgb_);
    bottom->addWidget(luma_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->setSpacing(3);
    layout->addWidget(view_);
    layout->addLayout(bottom);

    updateTriangles();
    placeTriangles();
}

void HistogramPanel::showHistogram(const OutputHistogram& histogram) {
    view_->setHistogram(histogram);
    updateTriangles();
}

void HistogramPanel::clear() {
    view_->setHistogram(std::nullopt);
    updateTriangles();
}

bool HistogramPanel::hasHistogram() const noexcept {
    return view_->hasHistogram();
}

void HistogramPanel::setPointerColour(std::optional<QRgb> colour) {
    readout_->setText(readoutText(colour));
}

void HistogramPanel::setClippingShown(bool shadows, bool highlights) {
    shadows_->setChecked(shadows);
    highlights_->setChecked(highlights);
    shadows_->update();
    highlights_->update();
}

bool HistogramPanel::clippingShown(ClipKind kind) const {
    return (kind == ClipKind::Shadows ? shadows_ : highlights_)->isChecked();
}

std::optional<QColor> HistogramPanel::triangleColour(ClipKind kind) const {
    return asTriangle(kind == ClipKind::Shadows ? shadows_ : highlights_)->lit();
}

HistogramChannels HistogramPanel::channels() const noexcept {
    return view_->channels();
}

QString HistogramPanel::readout() const {
    return readout_->text();
}

bool HistogramPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == view_ && event->type() == QEvent::Resize) {
        placeTriangles();
    }
    return QWidget::eventFilter(watched, event);
}

void HistogramPanel::placeTriangles() {
    const QRectF plot = view_->plotRect();
    shadows_->move(qRound(plot.left()) + triangleInset, qRound(plot.top()) + triangleInset);
    highlights_->move(qRound(plot.right()) - triangleInset - triangleSize,
                      qRound(plot.top()) + triangleInset);
    shadows_->raise();
    highlights_->raise();
}

void HistogramPanel::updateTriangles() {
    const auto& histogram = view_->histogram();
    const ClippedChannels clipped = histogram ? clippedChannels(*histogram) : ClippedChannels{};
    const struct {
        QToolButton* button;
        ChannelSet channels;
        ClipKind kind;
        QString idle;
    } triangles[] = {
        {shadows_, clipped.shadows, ClipKind::Shadows, tr("Show clipped shadows")},
        {highlights_, clipped.highlights, ClipKind::Highlights, tr("Show clipped highlights")}};
    for (const auto& triangle : triangles) {
        triangle.button->setEnabled(histogram.has_value());
        asTriangle(triangle.button)
            ->setLit(triangle.channels.any() ? std::optional<QColor>(clipColour(triangle.channels))
                                             : std::nullopt);
        const QString text = histogram
                                 ? clippedText(*histogram, triangle.kind) + QStringLiteral("\n") +
                                       tr("Click to show them on the photograph")
                                 : triangle.idle;
        triangle.button->setToolTip(text);
        triangle.button->setAccessibleDescription(text);
    }
}

} // namespace arraw::app
