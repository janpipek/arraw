#include "ThumbnailCache.h"

#include <SettingsJson.h>

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <system_error>
#include <vector>

namespace arraw::app {

namespace {

namespace fs = std::filesystem;

/// @brief Converts a path to a string Qt takes.
QString qtString(const fs::path& path) {
    return QString::fromStdU16String(path.u16string());
}

/// @brief Hashes the file's identity, plus an optional extra, into a key.
/// @param kind One letter that keeps the two kinds of key apart.
std::optional<std::string> hashKey(char kind, const fs::path& file, const std::string& extra) {
    std::error_code error;
    const fs::path absolute = fs::absolute(file, error).lexically_normal();
    if (error) {
        return std::nullopt;
    }
    const auto size = fs::file_size(absolute, error);
    if (error) {
        return std::nullopt;
    }
    const auto time = fs::last_write_time(absolute, error);
    if (error) {
        return std::nullopt;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto add = [&hash](const QByteArray& part) {
        // Length-prefixed, so that no two ingredient lists run into the same bytes.
        hash.addData(QByteArray::number(part.size()) + ':');
        hash.addData(part);
    };
    add(QByteArray(1, kind));
    add(qtString(absolute).toUtf8());
    add(QByteArray::number(static_cast<qulonglong>(size)));
    add(QByteArray::number(static_cast<qlonglong>(time.time_since_epoch().count())));
    add(QByteArray::fromStdString(extra));
    return QString::fromLatin1(hash.result().toHex()).toStdString();
}

/// @brief Sets a file's modification time to now; failure is of no consequence.
void touch(const QString& path) {
    QFile file(path);
    if (file.open(QIODevice::ReadWrite | QIODevice::ExistingOnly)) {
        file.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
    }
}

} // namespace

ThumbnailCache::ThumbnailCache(fs::path root, std::uint64_t capBytes)
    : root_(std::move(root)), capBytes_(capBytes) {}

fs::path ThumbnailCache::defaultRoot() {
    // Not CacheLocation, which would add the executable's name: the CLI may share this later.
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    return fs::path(base.toStdU16String()) / "arraw" / "thumbnails";
}

std::optional<std::string> ThumbnailCache::embeddedKey(const fs::path& file) {
    return hashKey('e', file, {});
}

std::optional<std::string> ThumbnailCache::developedKey(const fs::path& file,
                                                        const DevelopState& state) {
    // The one place that lists what a developed thumbnail depends on beyond the file.
    std::string ingredients = settingsToJson(state.settings);
    if (!state.localAdjustments.empty()) {
        // Only when there are masks, so the keys of mask-free photographs stay what they were.
        // The counter does not change the picture, so it is replaced by the smallest valid one.
        DevelopState masked = state;
        std::uint32_t next = 1;
        for (const LocalAdjustment& mask : masked.localAdjustments) {
            next = std::max(next, mask.id.value + 1);
        }
        masked.nextLocalAdjustmentId = LocalAdjustmentId{next};
        ingredients += localAdjustmentsToJson(masked);
    }
    return hashKey('d', file, ingredients);
}

fs::path ThumbnailCache::pathOf(const std::string& key) const {
    return root_ / key.substr(0, 2) / (key + ".jpg");
}

QImage ThumbnailCache::load(const std::string& key) const {
    if (key.size() < 2) {
        return {};
    }
    const fs::path path = pathOf(key);
    const QString name = qtString(path);
    QImage image;
    if (!QFile::exists(name)) {
        return {};
    }
    if (!image.load(name, "JPEG") || image.isNull()) {
        std::error_code ignored;
        fs::remove(path, ignored);
        return {};
    }
    touch(name);
    return image;
}

bool ThumbnailCache::store(const std::string& key, const QImage& thumbnail) const {
    if (thumbnail.isNull() || key.size() < 2) {
        return false;
    }
    const fs::path path = pathOf(key);
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
    QImage image = thumbnail;
    if (image.width() > maxEdge || image.height() > maxEdge) {
        image = image.scaled(maxEdge, maxEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    // JPEG has no alpha.
    image = image.convertToFormat(QImage::Format_RGB888);
    QSaveFile file(qtString(path));
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (!image.save(&file, "JPEG", jpegQuality)) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

namespace {

/// @brief One file of the cache.
struct Entry {
    fs::path path;
    std::uint64_t size = 0;
    fs::file_time_type used;
};

/// @brief Lists the files under a root; the JPEGs, and apart from them the leftovers.
void scan(const fs::path& root, std::vector<Entry>& entries, std::vector<fs::path>& leftovers) {
    std::error_code error;
    for (fs::recursive_directory_iterator
             it(root, fs::directory_options::skip_permission_denied, error),
         end;
         !error && it != end; it.increment(error)) {
        std::error_code entryError;
        if (!it->is_regular_file(entryError)) {
            continue;
        }
        if (it->path().extension() != ".jpg") {
            // QSaveFile's temporary files are named after the entry, with a suffix.
            if (it->path().filename().string().find(".jpg") != std::string::npos) {
                leftovers.push_back(it->path());
            }
            continue;
        }
        const auto size = it->file_size(entryError);
        const auto time = it->last_write_time(entryError);
        if (!entryError) {
            entries.push_back({it->path(), size, time});
        }
    }
}

} // namespace

int ThumbnailCache::prune() const {
    std::vector<Entry> entries;
    std::vector<fs::path> leftovers;
    scan(root_, entries, leftovers);
    std::error_code ignored;
    const auto stale = fs::file_time_type::clock::now() - std::chrono::hours(1);
    for (const fs::path& leftover : leftovers) {
        // A recent one may be a write in progress.
        if (fs::last_write_time(leftover, ignored) < stale) {
            fs::remove(leftover, ignored);
        }
    }
    std::uint64_t total = 0;
    for (const Entry& entry : entries) {
        total += entry.size;
    }
    if (total <= capBytes_) {
        return 0;
    }
    std::ranges::sort(entries, {}, &Entry::used);
    int removed = 0;
    for (const Entry& entry : entries) {
        if (total <= capBytes_) {
            break;
        }
        if (fs::remove(entry.path, ignored)) {
            total -= entry.size;
            ++removed;
        }
    }
    return removed;
}

std::uint64_t ThumbnailCache::sizeBytes() const {
    std::vector<Entry> entries;
    std::vector<fs::path> leftovers;
    scan(root_, entries, leftovers);
    std::uint64_t total = 0;
    for (const Entry& entry : entries) {
        total += entry.size;
    }
    return total;
}

} // namespace arraw::app
