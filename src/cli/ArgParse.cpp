#include "cli/ArgParse.h"
#include <CLI11.hpp>

namespace cli {

int parseArgs(CLI::App& app, const std::vector<std::string>& args, QString& message) {
    // Own the program name rather than pointing argv[0] at the App's own
    // `name_`: CLI11 assigns `name_ = argv[0]` for an unnamed App, and a
    // string assigned from a pointer into itself is not a trade worth making.
    const std::string program = app.get_name();

    // CLI11's vector overload consumes back-to-front; hand it argc/argv.
    std::vector<const char*> argv;
    argv.push_back(program.c_str());
    for (const std::string& a : args)
        argv.push_back(a.c_str());

    try {
        app.parse(int(argv.size()), argv.data());
    } catch (const CLI::CallForHelp&) {
        message = QString::fromStdString(app.help());
        return 0;
    } catch (const CLI::ParseError& e) {
        message = QStringLiteral("%1: %2").arg(QString::fromStdString(program), e.what());
        return 2;
    }
    return -1;
}

} // namespace cli
