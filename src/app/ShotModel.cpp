#include "ShotModel.h"

#include <Sidecar.h>

#include <QByteArray>
#include <QMetaObject>
#include <QStringList>
#include <QVariant>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>

namespace arraw::app {

namespace {

namespace fs = std::filesystem;

/// @brief Converts a path to a string Qt shows.
QString qtString(const fs::path& path) {
    return QString::fromStdU16String(path.u16string());
}

/// @brief What a loader job reads.
enum class JobKind {
    Marks,   ///< The sidecar's marks, for the first time.
    Exif,    ///< The EXIF of the primary.
    Recheck, ///< The sidecar's marks, only if its file changed since a stamp.
};

/// @brief One read for the loader thread.
struct Job {
    /// What to read.
    JobKind kind = JobKind::Marks;
    /// Folder generation it was queued in.
    std::uint64_t generation = 0;
    /// Primary file of the shot.
    fs::path primary;
    /// For a recheck, the stamp of the sidecar when it was last read.
    std::int64_t knownStamp = 0;
};

/// @brief Stamps the sidecar of a photograph; 0 when there is none.
std::int64_t sidecarStampOf(const fs::path& primary) {
    try {
        return fileStamp(sidecarPath(primary)).value_or(0);
    } catch (const std::exception&) {
        return 0;
    }
}

/// @brief Writes a number of an exposure without noise: `2.8`, `11`.
QString plainNumber(double value) {
    return QString::number(std::round(value * 10.0) / 10.0, 'g', 4);
}

/// @brief Writes a shutter time as a photographer reads it: `1/250 s`, `2 s`.
std::optional<QString> shutterText(const URational& time) {
    const double seconds = time.value();
    if (!(seconds > 0.0) || !std::isfinite(seconds)) {
        return std::nullopt;
    }
    if (seconds >= 1.0) {
        return plainNumber(seconds) + " s";
    }
    return "1/" + QString::number(std::llround(1.0 / seconds)) + " s";
}

/// @brief Writes the capture time as `YYYY-MM-DD HH:MM:SS`, from EXIF's `YYYY:MM:DD HH:MM:SS`.
QString dateText(const std::string& exifDate) {
    QString text = QString::fromStdString(exifDate).trimmed();
    if (text.size() >= 10 && text[4] == ':' && text[7] == ':') {
        text[4] = '-';
        text[7] = '-';
    }
    return text;
}

} // namespace

std::optional<std::int64_t> fileStamp(const std::filesystem::path& path) {
    std::error_code error;
    const auto time = fs::last_write_time(path, error);
    if (error) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(time.time_since_epoch().count());
}

QString shotTooltip(const Shot& shot, const std::optional<ExifInfo>& exif) {
    QStringList lines;
    lines << qtString(shot.primary.filename());
    lines << QString::fromStdString(formatLabel(shot));
    if (!exif) {
        return lines.join('\n');
    }
    if (exif->dateTimeOriginal) {
        lines << dateText(*exif->dateTimeOriginal);
    }
    QString make = QString::fromStdString(exif->make.value_or("")).trimmed();
    const QString model = QString::fromStdString(exif->model.value_or("")).trimmed();
    // Cameras often repeat the maker in the model ("Canon", "Canon EOS R5").
    if (model.startsWith(make, Qt::CaseInsensitive)) {
        make.clear();
    }
    if (const QString camera = (make + ' ' + model).trimmed(); !camera.isEmpty()) {
        lines << camera;
    }
    if (exif->lensModel && !exif->lensModel->empty()) {
        lines << QString::fromStdString(*exif->lensModel);
    }
    QStringList exposure;
    if (exif->photographicSensitivity) {
        exposure << "ISO " + QString::number(*exif->photographicSensitivity);
    }
    if (exif->exposureTime) {
        if (const auto shutter = shutterText(*exif->exposureTime)) {
            exposure << *shutter;
        }
    }
    if (exif->fNumber && exif->fNumber->value() > 0.0) {
        exposure << "f/" + plainNumber(exif->fNumber->value());
    }
    if (exif->focalLength && exif->focalLength->value() > 0.0) {
        exposure << plainNumber(exif->focalLength->value()) + " mm";
    } else if (exif->focalLengthIn35mmFilm) {
        exposure << QString::number(*exif->focalLengthIn35mmFilm) + " mm";
    }
    if (!exposure.isEmpty()) {
        lines << exposure.join("  ");
    }
    return lines.join('\n');
}

/// @brief What the loader thread read for one job.
struct ShotModel::Loaded {
    /// What was read.
    JobKind kind = JobKind::Marks;
    /// Folder generation of the job.
    std::uint64_t generation = 0;
    /// Primary file of the shot.
    fs::path primary;
    /// Marks read; nothing when the job read none (EXIF, or an unchanged sidecar).
    std::optional<PhotoMarks> marks{};
    /// Stamp of the sidecar before it was read; 0 when it is not there.
    std::int64_t stamp = 0;
    /// For a recheck, whether the sidecar had changed.
    bool changed = false;
    /// EXIF read; an empty one when the file had none.
    std::optional<ExifInfo> exif{};
};

/// @brief The one thread that reads sidecars and EXIF for a model.
///
/// Jobs for marks and rechecks go before jobs for EXIF, and each runs first in,
/// first out. Every job produces exactly one result, so the model can count
/// what it is owed.
class ShotModel::Loader {
public:
    /// @brief Starts the thread.
    /// @param deliver Receives each result on the loader thread; the caller marshals it. Must not
    /// throw.
    explicit Loader(std::function<void(Loaded)> deliver)
        : deliver_(std::move(deliver)),
          worker_([this](const std::stop_token& stop) { run(stop); }) {}

    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;
    Loader(Loader&&) = delete;
    Loader& operator=(Loader&&) = delete;

    /// @brief Stops the thread, after the file it is reading.
    ~Loader() {
        worker_.request_stop();
    }

    /// @brief Queues jobs.
    /// @param jobs Jobs to run, in order.
    void enqueue(std::vector<Job> jobs) {
        {
            const std::scoped_lock lock(mutex_);
            for (Job& job : jobs) {
                (job.kind == JobKind::Exif ? background_ : urgent_).push_back(std::move(job));
            }
        }
        wake_.notify_one();
    }

    /// @brief Drops the jobs not yet started; the running one finishes.
    void clear() {
        const std::scoped_lock lock(mutex_);
        urgent_.clear();
        background_.clear();
    }

private:
    /// @brief Serves jobs until asked to stop.
    void run(const std::stop_token& stop) {
        while (true) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                if (!wake_.wait(lock, stop,
                                [&] { return !urgent_.empty() || !background_.empty(); })) {
                    return;
                }
                auto& queue = urgent_.empty() ? background_ : urgent_;
                job = std::move(queue.front());
                queue.pop_front();
            }
            Loaded result = execute(job);
            if (stop.stop_requested()) {
                return;
            }
            deliver_(std::move(result));
        }
    }

    /// @brief Reads one job; a file that fails to read gives defaults, never an exception.
    static Loaded execute(const Job& job) {
        Loaded result{.kind = job.kind, .generation = job.generation, .primary = job.primary};
        if (job.kind == JobKind::Exif) {
            try {
                result.exif = readExif(job.primary);
            } catch (const std::exception&) {
                result.exif = ExifInfo{};
            }
            return result;
        }
        // The stamp is taken before the read, so a write in between shows up as a
        // change at the next check rather than being missed.
        result.stamp = sidecarStampOf(job.primary);
        if (job.kind == JobKind::Recheck) {
            if (result.stamp == job.knownStamp) {
                return result;
            }
            result.changed = true;
        }
        result.marks = PhotoMarks{};
        try {
            if (const auto contents = readSidecar(job.primary)) {
                result.marks = contents->marks;
            }
        } catch (const std::exception&) {
            // Unreadable: no marks. Writing refuses such a sidecar, so nothing is lost.
        }
        return result;
    }

    /// Receiver of results, called on the loader thread.
    std::function<void(Loaded)> deliver_;
    /// Guard of the two queues.
    std::mutex mutex_;
    /// Signal that a job is queued or the thread should stop.
    std::condition_variable_any wake_;
    /// Jobs that read sidecars.
    std::deque<Job> urgent_;
    /// Jobs that read EXIF.
    std::deque<Job> background_;
    /// Declared last so that everything above exists before it starts.
    std::jthread worker_;
};

ShotModel::ShotModel(QObject* parent)
    : QAbstractListModel(parent), loader_(std::make_unique<Loader>([this](Loaded result) {
          // Called on the loader thread. The queued call is dropped if the model is gone.
          QMetaObject::invokeMethod(
              this, [this, result = std::move(result)] { receive(result); }, Qt::QueuedConnection);
      })) {}

ShotModel::~ShotModel() {
    // Joins the thread before the members the callback touches go.
    loader_.reset();
}

void ShotModel::setFolder(const std::filesystem::path& folder) {
    std::vector<Shot> shots = listShots(folder);
    beginResetModel();
    loader_->clear();
    ++generation_;
    pending_ = 0;
    folder_ = folder;
    rows_.clear();
    rows_.reserve(shots.size());
    std::vector<fs::path> primaries;
    primaries.reserve(shots.size());
    for (Shot& shot : shots) {
        primaries.push_back(shot.primary);
        rows_.push_back(Row{.shot = std::move(shot)});
    }
    reindex();
    endResetModel();
    queueLoads(primaries);
}

void ShotModel::refresh() {
    if (folder_.empty()) {
        return;
    }
    std::vector<Shot> listing;
    try {
        listing = listShots(folder_);
    } catch (const std::runtime_error&) {
        // The folder went away: it has no shots now.
    }
    std::unordered_set<fs::path> wanted;
    for (const Shot& shot : listing) {
        wanted.insert(shot.primary);
    }
    for (int row = static_cast<int>(rows_.size()) - 1; row >= 0; --row) {
        if (!wanted.contains(rows_[row].shot.primary)) {
            beginRemoveRows({}, row, row);
            rows_.erase(rows_.begin() + row);
            endRemoveRows();
        }
    }
    // What is left is a subsequence of the listing, in the same order.
    std::vector<fs::path> added;
    for (int i = 0; i < static_cast<int>(listing.size()); ++i) {
        if (i < static_cast<int>(rows_.size()) && rows_[i].shot.primary == listing[i].primary) {
            if (rows_[i].shot != listing[i]) {
                rows_[i].shot = listing[i];
                changed(i, {CompanionsRole, FormatLabelRole, Qt::ToolTipRole});
            }
            continue;
        }
        added.push_back(listing[i].primary);
        beginInsertRows({}, i, i);
        rows_.insert(rows_.begin() + i, Row{.shot = listing[i]});
        endInsertRows();
    }
    reindex();
    std::vector<Job> rechecks;
    for (const Row& row : rows_) {
        if (row.marksLoaded && std::ranges::find(added, row.shot.primary) == added.end()) {
            rechecks.push_back({JobKind::Recheck, generation_, row.shot.primary, row.sidecarStamp});
        }
    }
    pending_ += static_cast<int>(rechecks.size());
    loader_->enqueue(std::move(rechecks));
    queueLoads(added);
}

int ShotModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant ShotModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }
    const Row& row = rows_[index.row()];
    switch (role) {
    case Qt::DisplayRole:
        return qtString(row.shot.primary.filename());
    case Qt::ToolTipRole:
        return shotTooltip(row.shot, row.exif);
    case PathRole:
        return qtString(row.shot.primary);
    case CompanionsRole: {
        QStringList companions;
        for (const fs::path& companion : row.shot.companions) {
            companions << qtString(companion);
        }
        return companions;
    }
    case FormatLabelRole:
        return QString::fromStdString(formatLabel(row.shot));
    case RatingRole:
        return row.marks.rating;
    case LabelRole:
        return row.marks.label ? static_cast<int>(*row.marks.label) : -1;
    case MarksLoadedRole:
        return row.marksLoaded;
    case ThumbnailRole:
        return row.thumbnail;
    default:
        return {};
    }
}

const Shot& ShotModel::shot(int row) const {
    return rows_.at(static_cast<std::size_t>(row)).shot;
}

int ShotModel::rowOf(const std::filesystem::path& primary) const {
    const auto found = index_.find(primary);
    return found == index_.end() ? -1 : found->second;
}

PhotoMarks ShotModel::marks(int row) const {
    return rows_.at(static_cast<std::size_t>(row)).marks;
}

const std::optional<ExifInfo>& ShotModel::exif(int row) const {
    return rows_.at(static_cast<std::size_t>(row)).exif;
}

void ShotModel::setMarks(const std::filesystem::path& primary, const PhotoMarks& marks) {
    const int index = rowOf(primary);
    if (index < 0) {
        return;
    }
    Row& row = rows_[index];
    row.marks = marks;
    row.marksLoaded = true;
    row.marksSet = true;
    changed(index, {RatingRole, LabelRole, MarksLoadedRole});
}

void ShotModel::setThumbnail(const std::filesystem::path& primary, QImage thumbnail) {
    const int index = rowOf(primary);
    if (index < 0) {
        return;
    }
    rows_[index].thumbnail = std::move(thumbnail);
    changed(index, {ThumbnailRole});
}

void ShotModel::receive(const Loaded& result) {
    if (result.generation != generation_) {
        return;
    }
    --pending_;
    if (const int index = rowOf(result.primary); index >= 0) {
        Row& row = rows_[index];
        switch (result.kind) {
        case JobKind::Exif:
            row.exif = result.exif;
            changed(index, {Qt::ToolTipRole});
            break;
        case JobKind::Marks:
            row.sidecarStamp = result.stamp;
            // Marks set by hand since the job was queued are newer than this reading.
            if (!row.marksSet && result.marks) {
                row.marks = *result.marks;
                row.marksLoaded = true;
                changed(index, {RatingRole, LabelRole, MarksLoadedRole});
            }
            break;
        case JobKind::Recheck:
            if (result.changed && result.marks) {
                const bool differs = row.marks != *result.marks || !row.marksLoaded;
                row.sidecarStamp = result.stamp;
                row.marks = *result.marks;
                row.marksLoaded = true;
                row.marksSet = false;
                if (differs) {
                    changed(index, {RatingRole, LabelRole, MarksLoadedRole});
                }
                emit sidecarRefreshed(qtString(result.primary), result.stamp);
            }
            break;
        }
    }
    if (pending_ == 0) {
        emit loadingFinished();
    }
}

void ShotModel::reindex() {
    index_.clear();
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        index_.emplace(rows_[i].shot.primary, i);
    }
}

void ShotModel::queueLoads(const std::vector<std::filesystem::path>& primaries) {
    std::vector<Job> jobs;
    jobs.reserve(primaries.size() * 2);
    for (const fs::path& primary : primaries) {
        jobs.push_back({JobKind::Marks, generation_, primary, 0});
    }
    for (const fs::path& primary : primaries) {
        jobs.push_back({JobKind::Exif, generation_, primary, 0});
    }
    pending_ += static_cast<int>(jobs.size());
    loader_->enqueue(std::move(jobs));
}

void ShotModel::changed(int row, const QList<int>& roles) {
    const QModelIndex index = this->index(row);
    emit dataChanged(index, index, roles);
}

} // namespace arraw::app
