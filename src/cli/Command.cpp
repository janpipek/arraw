#include "Command.h"

#include "ExportCommand.h"

#include <algorithm>
#include <array>

namespace arraw::cli {
namespace {

/// @brief Every command, implemented or merely reserved.
///
/// `info` and `preset` are named by docs/desired-features.md and carry no
/// implementation yet; a null `run` is what says so, rather than a stub that
/// prints an apology. Listing them means someone who types what the
/// documentation promised is told the feature is coming, not that they
/// mistyped -- and the names stay visibly taken.
constexpr std::array<Command, 3> table = {{
    {"export", "Render images through their develop settings and write them out.",
     &runExportCommand},
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

} // namespace arraw::cli
