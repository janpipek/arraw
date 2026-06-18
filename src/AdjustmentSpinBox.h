#pragma once
#include "FieldSpec.h"
#include <QDoubleSpinBox>

/**
 * Spin box that displays a develop adjustment through a FieldSpec.
 *
 * AdjustmentSpinBox centralises unit suffixes, positive signs, and tolerant
 * parsing for numeric editor rows. Formatting/parsing delegates to FieldSpec so
 * the conversion rules stay close to the adjustment definitions and tests.
 */
class AdjustmentSpinBox : public QDoubleSpinBox {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(AdjustmentSpinBox)
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
