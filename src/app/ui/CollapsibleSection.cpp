#include "CollapsibleSection.h"

#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>

namespace arraw::app {

namespace {

/// Arrow of a closed section, a triangle pointing right.
constexpr char16_t closedArrow = u'▸';
/// Arrow of an open section, a triangle pointing down.
constexpr char16_t openArrow = u'▾';

} // namespace

CollapsibleSection::CollapsibleSection(const QString& id, const QString& title, QWidget* parent)
    : QWidget(parent), id_(id), title_(title) {
    setObjectName(QStringLiteral("section_") + id);

    header_ = new QPushButton(this);
    header_->setObjectName("sectionHeader");
    header_->setFlat(true);
    // Reached by Tab, not taken by a click, so the photograph keeps its keys (ADR 040).
    header_->setFocusPolicy(Qt::TabFocus);
    header_->setCursor(Qt::PointingHandCursor);
    header_->setStyleSheet(QStringLiteral("QPushButton { text-align: left; font-weight: bold; "
                                          "padding: 3px 2px; border: none; }"));
    connect(header_, &QPushButton::clicked, this, [this] { setOpen(!open_); });

    body_ = new QWidget(this);
    body_->setObjectName("sectionBody");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header_);
    layout->addWidget(body_);

    open_ = QSettings().value(settingsKey(id), true).toBool();
    body_->setVisible(open_);
    updateHeader();
}

QString CollapsibleSection::settingsKey(const QString& id) {
    return QStringLiteral("developSections/") + id + QStringLiteral("/open");
}

void CollapsibleSection::setOpen(bool open) {
    if (open == open_) {
        return;
    }
    open_ = open;
    QSettings().setValue(settingsKey(id_), open_);
    body_->setVisible(open_);
    updateHeader();
    emit toggled(open_);
}

QSize CollapsibleSection::minimumSizeHint() const {
    QSize hint = QWidget::minimumSizeHint();
    hint.setWidth(std::max(header_->minimumSizeHint().width(), body_->minimumSizeHint().width()));
    return hint;
}

void CollapsibleSection::updateHeader() {
    // `&` in a title is a literal one, not a mnemonic.
    QString text = title_;
    text.replace('&', QStringLiteral("&&"));
    header_->setText(QString(QChar(open_ ? openArrow : closedArrow)) + QStringLiteral(" ") + text);
    header_->setAccessibleName(title_);
    header_->setAccessibleDescription(open_ ? tr("Open") : tr("Closed"));
}

} // namespace arraw::app
