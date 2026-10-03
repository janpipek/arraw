#include "Photo.h"

#include <DevelopState.h>
#include <Sidecar.h>

#include <stdexcept>
#include <utility>

using namespace arraw;

Photo::Photo(std::filesystem::path path, ImageMetadata metadata, DevelopState state,
             PhotoMarks marks)
    : path_(std::move(path)), metadata_(std::move(metadata)), state_(std::move(state)),
      marks_(marks) {
    validate(state_);
    if (marks_.rating < rejectedRating || marks_.rating > highestRating) {
        throw std::invalid_argument("rating must be from -1 to 5");
    }
}

Photo arraw::openPhoto(const std::filesystem::path& path, DiagnosticLog& log) {
    ImageMetadata metadata = readImageMetadata(path, log);
    try {
        if (auto sidecar = readSidecar(path, log)) {
            return {path, std::move(metadata), sidecar->state, sidecar->marks};
        }
    } catch (const std::runtime_error& error) {
        // An unreadable sidecar costs the photograph its state, not its
        // life. Nothing is lost by opening it bare: writeSidecar refuses to
        // replace a file it cannot read.
        log.record({.notice = Notice::SidecarUnreadable,
                    .severity = Severity::Error,
                    .subject = path,
                    .values = {std::string(error.what())}});
    }
    return {path, std::move(metadata)};
}
