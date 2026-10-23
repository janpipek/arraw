#include "EncodedForOutput.h"

#include <Develop.h>
#include <ImageBuffer.h>
#include <ImageImport.h>
#include <ImagePyramid.h>
#include <LocalAdjustments.h>
#include <SettingsJson.h>
#include <SnapshotThumbnails.h>

#include <QBuffer>
#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QString>

#include <algorithm>
#include <cstddef>
#include <format>
#include <stdexcept>
#include <system_error>
#include <utility>

using namespace arraw;

namespace {

namespace fs = std::filesystem;

/// Size of each end of the file that goes into the render key.
constexpr qint64 identityBytes = 64 * 1024;

/// Reads the first and last bytes of a file, which tell two photographs of the same name and size
/// apart without being tied to the path or the modification time.
/// @return The bytes, or nothing if the file cannot be read.
std::optional<QByteArray> contentIdentity(const fs::path& file) {
    QFile in(QString::fromStdU16String(file.u16string()));
    if (!in.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    QByteArray bytes = in.read(identityBytes);
    if (in.size() > identityBytes &&
        in.seek(std::max<qint64>(identityBytes, in.size() - identityBytes))) {
        bytes += in.read(identityBytes);
    }
    return bytes;
}

} // namespace

SnapshotRecipe arraw::recipeOf(const Snapshot& snapshot) {
    return {snapshot.id, snapshot.state, snapshot.negative};
}

std::optional<std::string> arraw::snapshotThumbnailKey(const fs::path& file,
                                                       const DevelopState& state,
                                                       const NegativeSettings& negative) {
    std::error_code error;
    const auto size = fs::file_size(file, error);
    if (error) {
        return std::nullopt;
    }
    const auto identity = contentIdentity(file);
    if (!identity) {
        return std::nullopt;
    }
    // The ingredients of ThumbnailCache::developedKey, except that the negative is always
    // included (this key space is new) and the lens corrections are not: a lens database that
    // differs between machines must not make a usable thumbnail stale.
    std::string ingredients = settingsToJson(state.settings);
    if (!state.localAdjustments.empty()) {
        // The id counter does not change the picture, so the smallest valid one stands for it.
        DevelopState masked = state;
        std::uint32_t next = 1;
        for (const LocalAdjustment& mask : masked.localAdjustments) {
            next = std::max(next, mask.id.value + 1);
        }
        masked.nextLocalAdjustmentId = LocalAdjustmentId{next};
        ingredients += localAdjustmentsToJson(masked, StrokeDetail::Digest);
    }
    ingredients += negativeToJson(negative);

    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto add = [&hash](const QByteArray& part) {
        // Length-prefixed, so that no two ingredient lists run into the same bytes.
        hash.addData(QByteArray::number(part.size()) + ':');
        hash.addData(part);
    };
    const std::u8string name = file.filename().u8string();
    add("snapshot-thumbnail");
    add(QByteArray::number(snapshotThumbnailRevision));
    add(QByteArray(reinterpret_cast<const char*>(name.data()),
                   static_cast<qsizetype>(name.size())));
    add(QByteArray::number(static_cast<qulonglong>(size)));
    add(*identity);
    add(QByteArray::fromStdString(ingredients));
    return QString::fromLatin1(hash.result().toHex()).toStdString();
}

bool arraw::hasCurrentThumbnail(const fs::path& file, const Snapshot& snapshot) {
    if (!snapshot.thumbnail) {
        return false;
    }
    const auto key = snapshotThumbnailKey(file, snapshot.state, snapshot.negative);
    return key && *key == snapshot.thumbnail->renderKey;
}

SnapshotThumbnail arraw::encodeSnapshotThumbnail(const QImage& image, std::string renderKey,
                                                 std::size_t byteLimit) {
    if (image.isNull()) {
        throw std::invalid_argument("A snapshot thumbnail cannot be made from a null image");
    }
    QImage fitted = image;
    if (fitted.width() > maximumSnapshotThumbnailSide ||
        fitted.height() > maximumSnapshotThumbnailSide) {
        fitted = fitted.scaled(maximumSnapshotThumbnailSide, maximumSnapshotThumbnailSide,
                               Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    // JPEG has no alpha.
    fitted = fitted.convertToFormat(QImage::Format_RGB888);
    for (const int quality : snapshotThumbnailQualities) {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        if (!fitted.save(&buffer, "JPEG", quality)) {
            throw std::runtime_error("Cannot encode a snapshot thumbnail as JPEG");
        }
        if (static_cast<std::size_t>(bytes.size()) >
            std::min(byteLimit, maximumSnapshotThumbnailBytes)) {
            continue;
        }
        SnapshotThumbnail thumbnail;
        const auto* begin = reinterpret_cast<const std::byte*>(bytes.constData());
        thumbnail.jpeg.assign(begin, begin + bytes.size());
        thumbnail.renderKey = std::move(renderKey);
        validate(thumbnail);
        return thumbnail;
    }
    throw std::runtime_error(
        std::format("A snapshot thumbnail does not fit {} bytes even at "
                    "quality {}",
                    maximumSnapshotThumbnailBytes,
                    snapshotThumbnailQualities[std::size(snapshotThumbnailQualities) - 1]));
}

std::optional<SnapshotThumbnail>
arraw::renderSnapshotThumbnail(const fs::path& file, const SnapshotRecipe& recipe,
                               const LensCorrection& lens, const std::function<bool()>& cancelled) {
    const auto stop = [&] { return cancelled && cancelled(); };
    auto key = snapshotThumbnailKey(file, recipe.state, recipe.negative);
    if (!key) {
        throw std::runtime_error("Cannot look at " + file.string());
    }
    if (stop()) {
        return std::nullopt;
    }
    ImageBuffer source = loadImage(file, discardedDiagnostics(), {.halfSize = true});
    if (stop()) {
        return std::nullopt;
    }
    RenderRequest request;
    request.size =
        RenderRequest::FitInside{static_cast<std::uint32_t>(maximumSnapshotThumbnailSide),
                                 static_cast<std::uint32_t>(maximumSnapshotThumbnailSide)};
    // Planned against the buffer: a half-size decode is not the size the metadata declares.
    const int level = pyramidLevelFor(source.size(), source.orientation(), recipe.state, request);
    for (int i = 0; i < level; ++i) {
        source = halved(source);
    }
    const ImageBuffer developed = develop(source, recipe.state, lens, request);
    if (stop()) {
        return std::nullopt;
    }
    const QImage display =
        encodedForOutput(developed, NamedEncoding::Srgb, QImage::Format_RGBA8888);
    return encodeSnapshotThumbnail(display, std::move(*key));
}
