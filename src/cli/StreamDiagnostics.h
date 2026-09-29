#pragma once

#include <Diagnostics.h>

#include <iosfwd>
#include <optional>
#include <string_view>

class QCommandLineParser;

namespace arraw::cli {

/// @brief How a log is written out.
enum class LogFormat {
    Text, ///< One sentence a person reads.
    Json, ///< One object per line, for whatever reads the output afterwards.
};

/// @brief Writes diagnostics to a stream as they happen.
///
/// Printing rather than collecting, because a batch that runs for an hour
/// should say what it is doing while it does it. The one writer every command
/// says things through, so `--log-format json` means the same everywhere
/// (ADR 006).
class StreamDiagnostics final : public DiagnosticLog {
public:
    /// @brief Constructs a log writing to @p stream.
    /// @param stream Stream the diagnostics are written to.
    /// @param format How each is written.
    /// @param quiet Whether to drop ::arraw::Severity::Info, keeping what went wrong.
    StreamDiagnostics(std::ostream& stream, LogFormat format, bool quiet = false)
        : stream_(stream), format_(format), quiet_(quiet) {}

    void record(const Diagnostic& diagnostic) override;

private:
    std::ostream& stream_;
    LogFormat format_;
    bool quiet_;
};

/// @brief Adds `--log-format` to a command's parser.
void addLogFormatOption(QCommandLineParser& parser);

/// @brief Reads `--log-format`, text when it is not given.
/// @param parser Parser the option was added to and that has parsed.
/// @param command Word that selects the command, for the usage error.
/// @param err Stream a usage error is written to.
/// @param code Set to ::arraw::cli::UsageError when the name is unknown.
/// @return The format, or `std::nullopt` after reporting an unknown one.
[[nodiscard]] std::optional<LogFormat> readLogFormat(const QCommandLineParser& parser,
                                                     std::string_view command, std::ostream& err,
                                                     int& code);

} // namespace arraw::cli
