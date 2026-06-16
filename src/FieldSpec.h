#pragma once
#include <QString>

// One source of truth for an adjustment slider's number handling: the integer
// QSlider range/default, how a raw slider tick maps to the stored param and to
// the human-readable display value, and how that value is formatted and parsed.
// Pure (no widget state) so it can be unit-tested without building the panel.
//
// Two independent scales, because they don't always agree:
//   param   = raw * paramScale     (what GlobalAdjustment stores)
//   display = raw * displayScale   (what the user reads/edits)
// e.g. HSL Hue stores ±100 (paramScale 1.0) but reads ±30° (displayScale 0.3).
struct FieldSpec {
    int min = -100;
    int max = 100;
    int def = 0;
    float paramScale = 1.0f;
    float displayScale = 1.0f;
    int decimals = 0;
    QString suffix = {};
    bool signedDisplay = false; // show a leading '+' on positive values
    float step = 1.0f;          // spinbox step, in display units

    // ── display space (what the spinbox shows / edits) ──────────────────────
    double displayMin() const { return min * double(displayScale); }

    double displayMax() const { return max * double(displayScale); }

    double rawToDisplay(int raw) const { return raw * double(displayScale); }

    int displayToRaw(double display) const;
    // Format a display value, e.g. 1.5 -> "+1.50 EV".
    QString formatDisplay(double value) const;
    // Extract the leading number from free-form text; *ok false if none.
    double parseDisplay(const QString& text, bool* ok = nullptr) const;

    // ── raw space (slider ticks) ────────────────────────────────────────────
    QString format(int raw) const { return formatDisplay(rawToDisplay(raw)); }

    int parse(const QString& text, bool* ok = nullptr) const;

    // ── param space (GlobalAdjustment storage) ──────────────────────────────
    float toParam(int raw) const { return raw * paramScale; }

    int fromParam(float param) const;
};
