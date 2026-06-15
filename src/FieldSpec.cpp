#include "FieldSpec.h"
#include <algorithm>
#include <cmath>
#include <QRegularExpression>

int FieldSpec::displayToRaw(double display) const {
    const int raw = int(std::lround(display / double(displayScale)));
    return std::clamp(raw, min, max);
}

QString FieldSpec::formatDisplay(double value) const {
    QString text = QString::number(value, 'f', decimals);
    if (signedDisplay && value > 0.0)
        text.prepend('+');
    return text + suffix;
}

double FieldSpec::parseDisplay(const QString& text, bool* ok) const {
    // Accept the leading signed number; ignore any trailing units the user
    // leaves in or omits ("+1.50 EV", "1.5", "  -2.00 EV  ").
    static const QRegularExpression number(QStringLiteral("[-+]?\\d*\\.?\\d+"));
    const auto match = number.match(text.trimmed());
    bool good = false;
    const double value = match.hasMatch() ? match.captured().toDouble(&good) : 0.0;
    if (ok)
        *ok = good;
    return good ? value : 0.0;
}

int FieldSpec::parse(const QString& text, bool* ok) const {
    bool good = false;
    const double value = parseDisplay(text, &good);
    if (!good) {
        if (ok)
            *ok = false;
        return def;
    }
    if (ok)
        *ok = true;
    return displayToRaw(value);
}

int FieldSpec::fromParam(float param) const {
    const int raw = int(std::lround(param / double(paramScale)));
    return std::clamp(raw, min, max);
}
