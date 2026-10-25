#include "Snapshots.h"

#include <LocalAdjustments.h>
#include <Sidecar.h>

#include <QBuffer>
#include <QByteArray>
#include <QImageReader>
#include <QSize>

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

using namespace arraw;

namespace {

/// @brief Last second of the year 9999, which the sidecar's ISO dates can spell.
constexpr std::int64_t lastSpellableSecond = 253402300799;

} // namespace

const Snapshot* SnapshotCollection::find(SnapshotId id) const noexcept {
    const auto found = std::ranges::find(entries, id, &Snapshot::id);
    return found == entries.end() ? nullptr : &*found;
}

SnapshotRevisionConflict::SnapshotRevisionConflict(std::uint64_t expected, std::uint64_t actual)
    : std::runtime_error(
          std::format("the snapshots changed: revision {} was expected, but the sidecar is at {}",
                      expected, actual)),
      expected(expected), actual(actual) {}

void arraw::validate(const SnapshotThumbnail& thumbnail) {
    if (thumbnail.renderKey.empty() ||
        thumbnail.renderKey.size() > maximumSnapshotThumbnailKeyBytes) {
        throw std::invalid_argument(
            std::format("a snapshot thumbnail's render key is 1 to {} bytes, not {}",
                        maximumSnapshotThumbnailKeyBytes, thumbnail.renderKey.size()));
    }
    // Round-tripping through the XML text node: no control characters (a carriage return would
    // read back as a line feed), no invalid UTF-8.
    if (storableMaskName(thumbnail.renderKey) != thumbnail.renderKey) {
        throw std::invalid_argument("a snapshot thumbnail's render key must be valid UTF-8 without "
                                    "control characters or characters XML cannot hold");
    }
    if (thumbnail.jpeg.empty() || thumbnail.jpeg.size() > maximumSnapshotThumbnailBytes) {
        throw std::invalid_argument(std::format("a snapshot thumbnail is 1 to {} bytes, not {}",
                                                maximumSnapshotThumbnailBytes,
                                                thumbnail.jpeg.size()));
    }
    // Only the header is read: a crafted file that declares huge dimensions is refused unseen.
    QByteArray bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(thumbnail.jpeg.data()),
                                               static_cast<qsizetype>(thumbnail.jpeg.size()));
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "JPEG");
    const QSize size = reader.size();
    if (!reader.canRead() || !size.isValid()) {
        throw std::invalid_argument("a snapshot thumbnail is not a JPEG with a readable header");
    }
    if (size.width() < 1 || size.height() < 1 || size.width() > maximumSnapshotThumbnailSide ||
        size.height() > maximumSnapshotThumbnailSide) {
        throw std::invalid_argument(
            std::format("a snapshot thumbnail is at most {0}x{0} pixels, not {1}x{2}",
                        maximumSnapshotThumbnailSide, size.width(), size.height()));
    }
}

void arraw::validate(const Snapshot& snapshot) {
    if (snapshot.id.value == 0) {
        throw std::invalid_argument("a snapshot's id must not be zero");
    }
    if (snapshot.parent && (snapshot.parent->value == 0 || *snapshot.parent == snapshot.id)) {
        throw std::invalid_argument("a snapshot's parent must be another snapshot");
    }
    if (snapshot.name.empty() || snapshot.name.size() > maximumSnapshotNameBytes) {
        throw std::invalid_argument(std::format("a snapshot's name is 1 to {} bytes, not {}",
                                                maximumSnapshotNameBytes, snapshot.name.size()));
    }
    if (storableMaskName(snapshot.name) != snapshot.name) {
        throw std::invalid_argument("a snapshot's name must be valid UTF-8 without control "
                                    "characters or characters XML cannot hold");
    }
    const std::int64_t seconds = snapshot.created.time_since_epoch().count();
    if (seconds < 0 || seconds > lastSpellableSecond) {
        throw std::invalid_argument("a snapshot's time must lie between 1970 and the year 9999");
    }
    validate(snapshot.state);
    validate(snapshot.negative);
    if (snapshot.thumbnail) {
        validate(*snapshot.thumbnail);
    }
}

void arraw::validate(const SnapshotCollection& collection) {
    if (collection.entries.size() > maximumSnapshots) {
        throw std::invalid_argument(std::format("a photograph keeps at most {} snapshots, not {}",
                                                maximumSnapshots, collection.entries.size()));
    }
    std::set<std::uint64_t> ids;
    for (const Snapshot& snapshot : collection.entries) {
        validate(snapshot);
        if (!ids.insert(snapshot.id.value).second) {
            throw std::invalid_argument(
                std::format("snapshot id {} is used twice", snapshot.id.value));
        }
        if (snapshot.id.value >= collection.nextId) {
            throw std::invalid_argument(
                std::format("the next snapshot id {} is not above snapshot id {}",
                            collection.nextId, snapshot.id.value));
        }
    }
    std::map<std::uint64_t, std::uint64_t> parents;
    for (const Snapshot& snapshot : collection.entries) {
        if (snapshot.parent) {
            if (!ids.contains(snapshot.parent->value)) {
                throw std::invalid_argument(
                    std::format("snapshot {} has a parent, {}, that does not exist",
                                snapshot.id.value, snapshot.parent->value));
            }
            parents[snapshot.id.value] = snapshot.parent->value;
        }
    }
    for (const Snapshot& snapshot : collection.entries) {
        std::uint64_t at = snapshot.id.value;
        for (std::size_t steps = 0; parents.contains(at); ++steps) {
            if (steps > collection.entries.size()) {
                throw std::invalid_argument(
                    std::format("snapshot {} is its own ancestor", snapshot.id.value));
            }
            at = parents.at(at);
        }
    }
}

OpenDocument arraw::openDocument(const std::filesystem::path& path, DiagnosticLog& log) {
    ImageMetadata metadata = readImageMetadata(path, log);
    try {
        if (auto sidecar = readSidecar(path, log)) {
            // A sidecar of marks alone, or another tool's, records no state.
            DevelopState state =
                sidecar->state ? *sidecar->state : defaultStateFor(metadata.encoding);
            const NegativeSettings negative =
                sidecar->negative.value_or(defaultNegativeFor(metadata));
            return {Photo(path, std::move(metadata), negative, std::move(state), sidecar->marks),
                    std::move(sidecar->snapshots), sidecar->savedBase,
                    sidecar->unreadableSnapshots};
        }
    } catch (const std::runtime_error& error) {
        // An unreadable sidecar costs the photograph its state, not its life. Nothing is lost by
        // opening it bare: the writers refuse to replace a file they cannot read.
        log.record({.notice = Notice::SidecarUnreadable,
                    .severity = Severity::Error,
                    .subject = path,
                    .values = {std::string(error.what())}});
    }
    return {Photo(path, std::move(metadata)), {}, std::nullopt, 0};
}

Photo arraw::openPhoto(const std::filesystem::path& path, DiagnosticLog& log) {
    return openDocument(path, log).photo;
}

Photo arraw::withSnapshot(const Photo& photo, const Snapshot& snapshot) {
    return Photo(photo.path(), photo.metadata(), snapshot.negative, snapshot.state, photo.marks());
}
