#include "ShotInputs.h"

#include "Command.h"

#include <PhotoMarks.h>
#include <Sidecar.h>

#include <QCommandLineParser>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <exception>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

using namespace arraw;

cli::ExpandedInputs cli::expandInputs(const std::vector<std::filesystem::path>& inputs,
                                      DiagnosticLog& log) {
    ExpandedInputs expanded;
    for (const auto& input : inputs) {
        std::error_code ignored;
        if (!std::filesystem::is_directory(input, ignored)) {
            expanded.photographs.push_back({input, std::nullopt});
            continue;
        }
        try {
            std::vector<Shot> shots = listShots(input);
            if (shots.empty()) {
                log.record({.notice = Notice::NoPhotographs, .subject = input});
            }
            for (Shot& shot : shots) {
                expanded.photographs.push_back({shot.primary, std::move(shot)});
            }
        } catch (const std::exception& problem) {
            log.record({.notice = Notice::InputFailed,
                        .severity = Severity::Error,
                        .subject = input,
                        .values = {std::string(problem.what())}});
            ++expanded.unreadableFolders;
        }
    }
    return expanded;
}

void cli::addMarksFilterOptions(QCommandLineParser& parser) {
    parser.addOption({"min-rating", "Only photographs rated at least N stars, 1 to 5.", "N"});
    parser.addOption({"rejected", "Only rejected photographs; not with --min-rating."});
    parser.addOption({"label",
                      "Only photographs with this colour label: red, yellow, green, blue or "
                      "purple. Repeat it to accept any of several.",
                      "name"});
}

std::optional<MarksFilter> cli::readMarksFilter(const QCommandLineParser& parser,
                                                std::string_view command, std::ostream& err,
                                                int& code) {
    MarksFilter filter;
    if (parser.isSet("min-rating")) {
        bool ok = false;
        const int rating = parser.value("min-rating").toInt(&ok);
        if (!ok || rating < 1 || rating > highestRating) {
            code = commandUsageError(err, command,
                                     "--min-rating: '" + parser.value("min-rating").toStdString() +
                                         "' is not a whole number from 1 to " +
                                         std::to_string(highestRating));
            return std::nullopt;
        }
        filter.minRating = rating;
    }
    filter.rejectsOnly = parser.isSet("rejected");
    if (filter.rejectsOnly && filter.minRating > 0) {
        code = commandUsageError(err, command, "--rejected cannot be combined with --min-rating");
        return std::nullopt;
    }
    for (const QString& name : parser.values("label")) {
        const auto found = std::ranges::find_if(colorLabelNames, [&name](const auto& entry) {
            return name.compare(QString::fromUtf8(entry.second.data(), entry.second.size()),
                                Qt::CaseInsensitive) == 0;
        });
        if (found == colorLabelNames.end()) {
            code = commandUsageError(err, command,
                                     "--label: '" + name.toStdString() +
                                         "' is not a colour label; expected red, yellow, green, "
                                         "blue or purple");
            return std::nullopt;
        }
        filter.labels.insert(found->first);
    }
    return filter;
}

bool cli::passesFilter(const MarksFilter& filter, const std::filesystem::path& photograph,
                       bool useSidecar) {
    if (!filter.isActive()) {
        return true;
    }
    PhotoMarks marks;
    if (useSidecar) {
        try {
            if (const auto contents = readSidecar(photograph)) {
                marks = contents->marks;
            }
        } catch (const std::runtime_error& error) {
            throw std::runtime_error(std::string(error.what()) +
                                     "; its marks are unknown, so the filter cannot be applied; "
                                     "fix the sidecar, or pass --no-sidecar");
        }
    }
    return filter.matches(marks);
}
