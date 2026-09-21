#pragma once

#include <filesystem>
#include <string>
#include <variant>
#include <vector>

namespace arraw {

/// @brief What a photographer should make of one diagnostic.
enum class Severity {
    Info,    ///< What happened, when it went as asked.
    Warning, ///< It worked, but not as the photograph deserved.
    Error,   ///< This photograph did not come out.
};

/// @brief Something worth telling a photographer about, by name.
///
/// The name rather than a sentence, because the sentence is presentation:
/// a log reader matches on this, a person reads what ::arraw::describe makes
/// of it, and a translation would replace only the latter.
enum class Notice {
    /// @brief A RAW declared no white balance, so one was substituted.
    SubstitutedWhiteBalance,

    /// @brief A photograph was rendered and written.
    Exported,

    /// @brief A photograph could not be rendered or written.
    InputFailed,

    /// @brief A batch reached its end, with a count of what did not come out.
    BatchFinished,
};

/// @brief One detail of a diagnostic, kept as a value rather than as prose.
using DiagnosticValue = std::variant<std::string, double>;

/// @brief One thing that happened, in a form both a person and a program can read.
struct Diagnostic {
    Notice notice;
    Severity severity = Severity::Info;

    /// @brief Photograph it concerns, empty when it concerns none.
    std::filesystem::path subject;

    /// @brief Details the notice needs to be specific.
    std::vector<DiagnosticValue> values;
};

/// @brief Writes a diagnostic as a sentence.
/// @param diagnostic Diagnostic to describe.
/// @return The sentence, without the subject or a severity label: a log
/// decides how to present those.
[[nodiscard]] std::string describe(const Diagnostic& diagnostic);

/// @brief Somewhere for diagnostics to go.
///
/// Passed to the operations rather than held by them, so that a caller decides
/// what becomes of them: printed as they happen, collected and collapsed, or
/// dropped. One thread's work goes to one log; a log shared between threads
/// has to be able to say so itself.
class DiagnosticLog {
public:
    DiagnosticLog() = default;
    DiagnosticLog(const DiagnosticLog&) = delete;
    DiagnosticLog& operator=(const DiagnosticLog&) = delete;
    DiagnosticLog(DiagnosticLog&&) = delete;
    DiagnosticLog& operator=(DiagnosticLog&&) = delete;
    virtual ~DiagnosticLog() = default;

    /// @brief Takes one diagnostic.
    /// @param diagnostic What happened.
    virtual void record(const Diagnostic& diagnostic) = 0;
};

/// @brief A log that throws everything away.
///
/// The default for callers that do not want to hear, so that nothing has to
/// check whether it has somewhere to report to.
/// @return A log shared by every caller that wants none.
[[nodiscard]] DiagnosticLog& discardedDiagnostics();

/// @brief A log that keeps what it is given.
class CollectedDiagnostics final : public DiagnosticLog {
public:
    void record(const Diagnostic& diagnostic) override;

    /// @brief Diagnostics recorded so far, oldest first.
    [[nodiscard]] const std::vector<Diagnostic>& entries() const noexcept {
        return entries_;
    }

private:
    std::vector<Diagnostic> entries_;
};

} // namespace arraw
