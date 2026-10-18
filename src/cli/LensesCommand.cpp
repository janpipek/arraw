#include "LensesCommand.h"

#include "Command.h"
#include "TerminalStyle.h"

#include <LensDatabase.h>

#include <QCommandLineParser>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

using namespace arraw;

namespace {

/// @brief Reports a usage problem of `lenses`.
int usageError(std::ostream& err, const std::string& message) {
    return cli::commandUsageError(err, "lenses", message);
}

/// @brief Declares the options and help of `lenses`.
void configure(QCommandLineParser& parser) {
    parser.setApplicationDescription(
        "List the lenses of the lens database, read-only.\n"
        "\n"
        "Without options, every lens, one per line: its name, its mounts, the crop factor its\n"
        "profile was measured on and its focal range. --search lists the lenses whose maker and\n"
        "model hold every word given, in any order and case; --camera lists the lenses that fit\n"
        "the named camera's mount, the camera being written as its EXIF does (\"Canon EOS 80D\").\n"
        "--info prints where the database was loaded from, its stamp, and how many cameras and\n"
        "lenses it holds. The database is lensfun's, found as the application finds it (a copy\n"
        "bundled beside the program, else the system's, the user's update over either when "
        "newer),\n"
        "unless --database names a directory of lensfun XML files.\n"
        "The exit status is 0 on success, 1 when no database could be loaded or the camera is not\n"
        "in it, 2 for a usage error.");
    parser.addHelpOption();
    parser.addOption({"database", "Directory of lensfun XML files to use instead.", "dir"});
    parser.addOption({"camera", "Show only lenses that fit this camera's mount.", "text"});
    parser.addOption({"search", "Show only lenses whose name holds all of these words.", "text"});
    parser.addOption(
        {"info", "Show the database's directory, stamp and counts instead of lenses."});
}

/// @brief Spells a number without trailing zeros.
std::string plain(double value) {
    std::string text = std::to_string(value);
    text.erase(text.find_last_not_of('0') + 1);
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

/// @brief Writes one lens as a line.
void write(std::ostream& out, const LensEntry& lens) {
    out << lens.model;
    // lensfun's model usually starts with the maker already; say it when it does not.
    if (lens.model.find(lens.maker) == std::string::npos) {
        out << " (" << lens.maker << ")";
    }
    out << "  [";
    for (std::size_t index = 0; index < lens.mounts.size(); ++index) {
        out << (index > 0 ? ", " : "") << lens.mounts[index];
    }
    out << "; crop " << plain(lens.cropFactor);
    if (lens.maxFocal > 0.0) {
        out << "; " << plain(lens.minFocal);
        if (lens.maxFocal != lens.minFocal) {
            out << "-" << plain(lens.maxFocal);
        }
        out << " mm";
    }
    out << "]\n";
}

/// @brief Reports why a database is missing.
void explain(std::ostream& err, const LensDatabase& database) {
    err << cli::accented(err, "error:", cli::Accent::Error) << " ";
    if (database.problem() == LensProblem::LensfunUnavailable) {
        err << "this arraw was built without lensfun; there are no lenses to list\n";
    } else {
        err << "no lens database could be loaded"
            << (database.directory().empty() ? "" : " from " + database.directory().string())
            << "\n";
    }
}

int list(const QCommandLineParser& parser, std::ostream& out, std::ostream& err) {
    const auto database = parser.isSet("database")
                              ? std::optional<LensDatabase>(LensDatabase::fromDirectory(
                                    parser.value("database").toStdString()))
                              : std::nullopt;
    const LensDatabase& lenses = database ? *database : LensDatabase::system();
    if (!lenses.loaded()) {
        explain(err, lenses);
        return cli::Failed;
    }
    if (parser.isSet("info")) {
        out << "lensfun: " << lensfunVersion() << "\n"
            << "Database: " << lenses.directory().string() << "\n"
            << "Stamp: " << lenses.stamp() << "\n"
            << "Cameras: " << lenses.cameraCount() << "\n"
            << "Lenses: " << lenses.lensCount() << "\n";
        return cli::Success;
    }
    std::optional<std::string> mount;
    if (parser.isSet("camera")) {
        const std::string text = parser.value("camera").toStdString();
        // The EXIF's maker is the first word of its model for the cameras that repeat it; lensfun
        // finds a camera by its words, so the whole text serves as the model.
        const auto camera = lenses.findCamera("", text);
        if (!camera) {
            err << cli::accented(err, "error:", cli::Accent::Error) << " no camera \"" << text
                << "\" in the database\n";
            return cli::Failed;
        }
        mount = camera->mount;
        out << (camera->model.starts_with(camera->maker) ? "" : camera->maker + " ")
            << camera->model << ": mount " << camera->mount << ", crop "
            << plain(camera->cropFactor) << "\n";
    }
    for (const LensEntry& lens : lenses.lenses(parser.value("search").toStdString(), mount)) {
        write(out, lens);
    }
    return cli::Success;
}

} // namespace

int cli::runLensesCommand(const QStringList& arguments, std::ostream& out, std::ostream& err,
                          const StartApplication& start) {
    QCommandLineParser parser;
    configure(parser);
    if (!parser.parse(arguments)) {
        return usageError(err, parser.errorText().toStdString());
    }
    if (parser.isSet("help")) {
        writeStyledHelp(out, commandHelp(parser));
        return Success;
    }
    if (!parser.positionalArguments().isEmpty()) {
        return usageError(err, "lenses takes no arguments; use --search to filter");
    }
    start(ApplicationKind::Core);
    return list(parser, out, err);
}
