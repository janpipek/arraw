#pragma once
#include "FieldSpec.h"
#include <QDoubleSpinBox>

// A QDoubleSpinBox that presents an adjustment in its FieldSpec's display units:
// conditional '+' sign on positives, units suffix, and tolerant parsing (the
// user may leave the suffix in or drop it). All formatting/parsing delegates to
// the FieldSpec so the rules live in exactly one place (and stay unit-tested).
class AdjustmentSpinBox : public QDoubleSpinBox {
    Q_OBJECT
public:
    explicit AdjustmentSpinBox(const FieldSpec& spec, QWidget* parent = nullptr);

    const FieldSpec& spec() const { return fieldSpec; }

protected:
    QString textFromValue(double value) const override;
    double valueFromText(const QString& text) const override;
    QValidator::State validate(QString& input, int& pos) const override;

private:
    FieldSpec fieldSpec;
};
