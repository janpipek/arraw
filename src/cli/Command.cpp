#include "Command.h"

#include "Cli.h"
#include "ExportCommand.h"
#include "GpuTestCommand.h"

#include <algorithm>
#include <array>
#include <ostream>

namespace arraw::cli {
namespace {

/// @brief Every command, implemented or merely reserved.
///
/// `info` and `preset` are named by docs/desired-features.md and carry no
/// implementation yet; a null `run` is what says so, rather than a stub that
/// prints an apology. Listing them means someone who types what the
/// documentation promised is told the feature is coming, not that they
/// mistyped -- and the names stay visibly taken.
constexpr std::array<Command, 4> table = {{
    {"export", "Render images through their develop settings and write them out.",
     &runExportCommand},
    {"gpu-test", "Check that the GPU backend works on this machine.", &runGpuTestCommand},
    {"info", "Show camera metadata and edit state, read-only.", nullptr},
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

int commandUsageError(std::ostream& err, std::string_view command, std::string_view message) {
    err << "error: " << message << "\n\nTry 'arraw-cli " << command << " --help'.\n";
    return UsageError;
}

} // namespace arraw::cli
