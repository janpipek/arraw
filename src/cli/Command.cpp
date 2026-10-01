#include "Command.h"

#include "Cli.h"
#include "ExportCommand.h"
#include "GpuTestCommand.h"
#include "InfoCommand.h"

#include <QCommandLineParser>
#include <QString>

#include <algorithm>
#include <array>
#include <ostream>
#include <string>
#include <string_view>

namespace arraw::cli {
namespace {

/// @brief Every command, implemented or merely reserved.
///
/// `preset` is named by docs/desired-features.md and carries no
/// implementation yet; a null `run` is what says so, rather than a stub that
/// prints an apology. Listing them means someone who types what the
/// documentation promised is told the feature is coming, not that they
/// mistyped -- and the names stay visibly taken.
constexpr std::array<Command, 4> table = {{
    {"export", "Render images through their develop settings and write them out.",
     &runExportCommand},
    {"gpu-test", "Check that the GPU backend works on this machine.", &runGpuTestCommand},
    {"info", "Show camera metadata and edit state, read-only.", &runInfoCommand},
    {"preset", "List, show, and apply saved presets.", nullptr},
}};

} // namespace

std::span<const Command> commands() {
    return table;
}

const Command* findCommand(std::string_view name) {
    const auto found = std::ranges::find_if(
        table, [name](const Command& command) { return command.name == name; });
    return found == table.end() ? nullptr : &*found;
}

std::string commandHelp(const QCommandLineParser& parser) {
    std::string text = parser.helpText().toStdString();
    // "Usage: <program> ...": whatever Qt put there, up to the next space.
    constexpr std::string_view usage = "Usage: ";
    if (text.starts_with(usage)) {
        const auto end = text.find(' ', usage.size());
        if (end != std::string::npos) {
            text.replace(usage.size(), end - usage.size(), "arraw-cli");
        }
    }
    return text;
}

int commandUsageError(std::ostream& err, std::string_view command, std::string_view message) {
    err << "error: " << message << "\n\nTry 'arraw-cli " << command << " --help'.\n";
    return UsageError;
}

} // namespace arraw::cli
