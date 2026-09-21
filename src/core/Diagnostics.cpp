#include "Diagnostics.h"

#include <sstream>
#include <utility>

using namespace arraw;

namespace {

/// @brief Writes one detail, whichever kind it is.
std::string text(const DiagnosticValue& value) {
    if (const auto* words = std::get_if<std::string>(&value)) {
        return *words;
    }
    std::ostringstream stream;
    stream << std::get<double>(value);
    return stream.str();
}

/// @brief Reads one detail, or an empty one when it was not supplied.
std::string valueAt(const Diagnostic& diagnostic, std::size_t index) {
    return index < diagnostic.values.size() ? text(diagnostic.values[index]) : std::string{};
}

/// @brief A log with nowhere to put anything.
class DiscardingDiagnostics final : public DiagnosticLog {
public:
    void record(const Diagnostic&) override {}
};

} // namespace

std::string arraw::describe(const Diagnostic& diagnostic) {
    // Exhaustive and without a default, so that a notice added without
    // anything to say about it fails the build rather than the reader.
    switch (diagnostic.notice) {
    case Notice::SubstitutedWhiteBalance:
        return "this file records no white balance, so a daylight one was used; "
               "the temperature shown is an estimate, not what the camera saw";
    case Notice::Exported:
        return "written to " + valueAt(diagnostic, 0);
    case Notice::InputFailed:
        return valueAt(diagnostic, 0);
    case Notice::BatchFinished:
        return valueAt(diagnostic, 0) + " of " + valueAt(diagnostic, 1) + " failed";
    }
    return {};
}

DiagnosticLog& arraw::discardedDiagnostics() {
    static DiscardingDiagnostics log;
    return log;
}

void CollectedDiagnostics::record(const Diagnostic& diagnostic) {
    entries_.push_back(diagnostic);
}
