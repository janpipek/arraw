#pragma once

#include <Diagnostics.h>

namespace arraw::app {

/// @brief Writes diagnostics to Qt's debug output as they happen.
///
/// A stand-in until the window has somewhere to show them, such as a status
/// bar: each severity goes to the Qt message type that matches it, so a
/// message handler or `QT_LOGGING_RULES` can filter them like any other.
class DebugDiagnostics final : public DiagnosticLog {
public:
    void record(const Diagnostic& diagnostic) override;
};

} // namespace arraw::app
