// ANSI seasoning for the CLI's human-readable tables. Whether to colour is
// decided once at the process edge (`TextStyle::forStdout()`, called from
// main) and passed down; call sites never sniff the terminal themselves. They
// can't: the stream a command writes to is a real console in production and a
// plain QString under test, and only the edge can tell those apart. A
// default-constructed TextStyle emits plain text, so tests, pipes and
// redirects get exactly the bytes they got before colour existed.

#pragma once

#include <cstdint>
#include <QString>

namespace cli {

enum class Ink : std::uint8_t {
    Bold,
    Dim,
    Red,
    Green,
    Yellow,
    Blue,
    Magenta,
    Cyan,
};

class TextStyle {
public:
    // Plain text: the safe default every non-terminal sink wants.
    TextStyle() = default;

    explicit TextStyle(bool enabled)
        : enabled_(enabled) {}

    // Colour only when stdout is a terminal that wants escapes — see the
    // guards in TextStyle.cpp. Call once, at startup, before any output.
    static TextStyle forStdout();

    bool enabled() const { return enabled_; }

    // `text` wrapped in the escape for `ink` and reset afterwards; returned
    // unchanged when disabled or empty (an escape around nothing is noise).
    QString paint(const QString& text, Ink ink) const;

    QString bold(const QString& text) const { return paint(text, Ink::Bold); }

    QString dim(const QString& text) const { return paint(text, Ink::Dim); }

private:
    bool enabled_ = false;
};

} // namespace cli
