#include "AdjustmentSpinBox.h"

AdjustmentSpinBox::AdjustmentSpinBox(const FieldSpec& spec, QWidget* parent)
    : QDoubleSpinBox(parent), fieldSpec(spec)
{
    setDecimals(spec.decimals);
    setRange(spec.displayMin(), spec.displayMax());
    setSingleStep(spec.step);
    setButtonSymbols(QAbstractSpinBox::NoButtons);   // arrows hidden; ↑↓/scroll still work
    setKeyboardTracking(false);                      // commit on Enter / focus-out
    setAlignment(Qt::AlignRight | Qt::AlignVCenter);
}

QString AdjustmentSpinBox::textFromValue(double value) const {
    return fieldSpec.formatDisplay(value);   // includes sign + suffix
}

double AdjustmentSpinBox::valueFromText(const QString& text) const {
    bool ok = false;
    const double parsed = fieldSpec.parseDisplay(text, &ok);
    return ok ? parsed : value();
}

QValidator::State AdjustmentSpinBox::validate(QString& input, int& /*pos*/) const {
    QString s = input.trimmed();
    const QString unit = fieldSpec.suffix.trimmed();
    if (!unit.isEmpty() && s.endsWith(unit))
        s.chop(unit.size());
    s = s.trimmed();
    if (s.isEmpty() || s == "+" || s == "-")
        return QValidator::Intermediate;   // mid-typing
    bool ok = false;
    s.toDouble(&ok);
    return ok ? QValidator::Acceptable : QValidator::Invalid;
}
