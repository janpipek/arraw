#pragma once

#include <Diagnostics.h>

namespace arraw::app {

class DiagnosticModel;

/// @brief Logging category the diagnostics are echoed to Qt's debug output in.
inline constexpr char diagnosticsCategory[] = "arraw.diagnostics";

/// @brief Keeps diagnostics for the debug window, and echoes them to Qt's debug output.
///
/// The echo goes to the Qt message type matching each severity, in
/// diagnosticsCategory, so `QT_LOGGING_RULES` or a message handler can filter
/// them like any other message.
class DebugDiagnostics final : public DiagnosticLog {
public:
    /// @brief Makes a log that keeps what it is given in a table.
    /// @param model Table to keep diagnostics in; must outlive the log.
    explicit DebugDiagnostics(DiagnosticModel& model) : model_(model) {}

    void record(const Diagnostic& diagnostic) override;

private:
    DiagnosticModel& model_;
};

} // namespace arraw::app
