#include "ShotModel.h"

#include "FolderDiff.h"

#include <Sidecar.h>

#include <QByteArray>
#include <QMetaObject>
#include <QStringList>
#include <QVariant>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <iterator>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>
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

/// @brief Spells a folder one way, so that the watcher's spelling and the listing's agree.
fs::path folderKey(const fs::path& folder) {
    fs::path key = folder.lexically_normal();
    return key.has_filename() ? key : key.parent_path();
}

} // namespace

/// @brief One read for the loader thread.
struct ShotModel::Job {
    /// What to read.
    JobKind kind = JobKind::Marks;
    /// Folder generation it was queued in.
    std::uint64_t generation = 0;
    /// Primary file of the shot.
    fs::path primary;
    /// For a recheck, the stamp of the sidecar when it was last read.
    std::int64_t knownStamp = 0;
};

std::optional<std::int64_t> fileStamp(const std::filesystem::path& path) {
    std::error_code error;
    const auto time = fs::last_write_time(path, error);
    if (error) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(time.time_since_epoch().count());
}

QString shotTooltip(const Shot& shot, const std::optional<ExifInfo>& exif, bool hasSidecar,
                    const std::filesystem::path& shownName) {
    QStringList lines;
    lines << qtString(shownName.empty() ? shot.primary.filename() : shownName);
    lines << QString::fromStdString(formatLabel(shot));
    if (hasSidecar) {
        lines << QObject::tr("Has an XMP sidecar");
    }
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

void ShotModel::setFolder(const std::filesystem::path& folder, bool recursive) {
    // Only opened, not read: a folder that cannot be opened is refused at once, and the reading
    // is the walk's (plan, answer 4).
    {
        const fs::directory_iterator probe(folder);
    }
    beginResetModel();
    loader_->clear();
    ++generation_;
    ++walk_;
    pending_ = 0;
    folder_ = folder;
    recursive_ = recursive;
    rows_.clear();
    spans_.clear();
    index_.clear();
    seen_.clear();
    recheckAll_ = false;
    dirtyFolders_.clear();
    refreshWanted_ = false;
    wantedAll_ = false;
    wantedFolders_.clear();
    walking_ = Walking::Listing;
    endResetModel();
    emit listingChanged(true);
    emit walkRequested();
}

std::filesystem::path ShotModel::shownName(const Shot& shot) const {
    if (!recursive_) {
        return {};
    }
    std::filesystem::path relative = shot.primary.lexically_relative(folder_);
    if (!relative.has_parent_path() || *relative.begin() == "..") {
        return {};
    }
    // Shown with the separator of the platform.
    return relative.make_preferred();
}

void ShotModel::refresh(const std::optional<std::vector<fs::path>>& dirty) {
    if (folder_.empty()) {
        return;
    }
    if (dirty) {
        for (const fs::path& folder : *dirty) {
            wantedFolders_.insert(folderKey(folder));
        }
    } else {
        wantedAll_ = true;
    }
    refreshWanted_ = true;
    if (walking_ == Walking::None) {
        startRefresh();
    }
}

void ShotModel::startRefresh() {
    refreshWanted_ = false;
    recheckAll_ = std::exchange(wantedAll_, false);
    dirtyFolders_ = std::exchange(wantedFolders_, {});
    ++walk_;
    seen_.clear();
    walking_ = Walking::Refresh;
    emit walkRequested();
}

void ShotModel::applyFolders(std::uint64_t walk, const std::vector<FolderResult>& batch) {
    if (walk != walk_ || walking_ == Walking::None) {
        return;
    }
    std::vector<fs::path> added;
    std::vector<Job> rechecks;
    for (const FolderResult& result : batch) {
        applyFolder(result, added, rechecks);
    }
#ifndef NDEBUG
    // The one comparator must agree with the walk (plan, Risks): spans tile the rows in order.
    int next = 0;
    for (const auto& [folder, span] : spans_) {
        assert(span.first == next && span.count > 0);
        next = span.first + span.count;
    }
    assert(next == static_cast<int>(rows_.size()));
#endif
    pending_ += static_cast<int>(rechecks.size());
    loader_->enqueue(std::move(rechecks));
    queueLoads(added);
}

void ShotModel::finishListing(std::uint64_t walk, const ListingSummary& /*summary*/) {
    if (walk != walk_ || walking_ == Walking::None) {
        return;
    }
    // What the walk did not read is gone, or cannot be read: it has no shots now. From the last
    // folder back, so that each removal moves the fewest spans.
    std::vector<fs::path> unseen;
    for (const auto& [folder, span] : spans_) {
        if (!seen_.contains(folder)) {
            unseen.push_back(folder);
        }
    }
    for (auto folder = unseen.rbegin(); folder != unseen.rend(); ++folder) {
        removeFolder(spans_.find(*folder));
    }
    seen_.clear();
    const bool wasListing = walking_ == Walking::Listing;
    walking_ = Walking::None;
    recheckAll_ = false;
    dirtyFolders_.clear();
    if (wasListing) {
        emit listingChanged(false);
    }
    if (refreshWanted_ && walking_ == Walking::None) {
        startRefresh();
        return;
    }
    reportIfLoaded();
}

void ShotModel::applyFolder(const FolderResult& result, std::vector<fs::path>& added,
                            std::vector<Job>& rechecks) {
    const fs::path folder = withoutTrailingSeparator(result.folder);
    seen_.insert(folder);
    const std::span<const Shot> fresh(result.shots);
    const auto found = spans_.find(folder);
    if (found == spans_.end()) {
        if (!fresh.empty()) {
            insertFolder(folder, fresh, added);
        }
        return;
    }
    const Span span = found->second;
    const FolderDiff diff = diffFolder(
        static_cast<std::size_t>(span.count),
        [&](std::size_t i) -> const Shot& { return rows_[span.first + i].shot; }, fresh);
    if (!diff.changed()) {
        // Unchanged rows; only their sidecars may be due for a look.
    } else if (diff.needsReset) {
        resetFolder(span, fresh, added);
    } else {
        applyDiff(span.first, diff, fresh, added);
    }
    const int count = static_cast<int>(fresh.size());
    shiftSpans(std::next(found), count - span.count);
    if (count == 0) {
        spans_.erase(found);
        return;
    }
    found->second.count = count;
    if (recheckAll_ || diff.changed() || dirtyFolders_.contains(folderKey(folder))) {
        for (int row = span.first; row < span.first + count; ++row) {
            const Row& entry = rows_[row];
            if (entry.marksLoaded) {
                rechecks.push_back(
                    {JobKind::Recheck, generation_, entry.shot.primary, entry.sidecarStamp});
            }
        }
    }
}

void ShotModel::insertFolder(const fs::path& folder, std::span<const Shot> shots,
                             std::vector<fs::path>& added) {
    const auto next = spans_.lower_bound(folder);
    const int first = next == spans_.end() ? static_cast<int>(rows_.size()) : next->second.first;
    const int count = static_cast<int>(shots.size());
    std::vector<Row> fresh;
    fresh.reserve(shots.size());
    for (const Shot& shot : shots) {
        fresh.push_back(Row{.shot = shot});
        added.push_back(shot.primary);
    }
    beginInsertRows({}, first, first + count - 1);
    rows_.insert(rows_.begin() + first, std::make_move_iterator(fresh.begin()),
                 std::make_move_iterator(fresh.end()));
    // The model is whole again before views hear of the rows: they may look a shot up.
    shiftSpans(next, count);
    spans_.emplace_hint(next, folder, Span{first, count});
    reindexFrom(first);
    endInsertRows();
}

void ShotModel::applyDiff(int first, const FolderDiff& diff, std::span<const Shot> fresh,
                          std::vector<fs::path>& added) {
    for (const RowRun& run : diff.removes) {
        const int from = first + run.first;
        beginRemoveRows({}, from, from + run.count - 1);
        for (int row = from; row < from + run.count; ++row) {
            index_.erase(rows_[row].shot.primary);
        }
        rows_.erase(rows_.begin() + from, rows_.begin() + from + run.count);
        reindexFrom(from);
        endRemoveRows();
    }
    for (const RowRun& run : diff.inserts) {
        const int from = first + run.first;
        std::vector<Row> rows;
        rows.reserve(static_cast<std::size_t>(run.count));
        for (int k = 0; k < run.count; ++k) {
            const Shot& shot = fresh[static_cast<std::size_t>(run.first + k)];
            rows.push_back(Row{.shot = shot});
            added.push_back(shot.primary);
        }
        beginInsertRows({}, from, from + run.count - 1);
        rows_.insert(rows_.begin() + from, std::make_move_iterator(rows.begin()),
                     std::make_move_iterator(rows.end()));
        reindexFrom(from);
        endInsertRows();
    }
    for (const int k : diff.updates) {
        const int row = first + k;
        rows_[row].shot = fresh[static_cast<std::size_t>(k)];
        changed(row, {CompanionsRole, FormatLabelRole, Qt::ToolTipRole});
    }
}

void ShotModel::resetFolder(Span span, std::span<const Shot> fresh, std::vector<fs::path>& added) {
    beginResetModel();
    std::unordered_map<fs::path, int> old;
    old.reserve(static_cast<std::size_t>(span.count));
    for (int row = span.first; row < span.first + span.count; ++row) {
        old.emplace(rows_[row].shot.primary, row);
        index_.erase(rows_[row].shot.primary);
    }
    std::vector<Row> next;
    next.reserve(fresh.size());
    for (const Shot& shot : fresh) {
        if (const auto found = old.find(shot.primary); found != old.end()) {
            Row row = std::move(rows_[static_cast<std::size_t>(found->second)]);
            row.shot = shot;
            next.push_back(std::move(row));
        } else {
            next.push_back(Row{.shot = shot});
            added.push_back(shot.primary);
        }
    }
    rows_.erase(rows_.begin() + span.first, rows_.begin() + span.first + span.count);
    rows_.insert(rows_.begin() + span.first, std::make_move_iterator(next.begin()),
                 std::make_move_iterator(next.end()));
    reindexFrom(span.first);
    endResetModel();
}

void ShotModel::removeFolder(Spans::iterator span) {
    const Span rows = span->second;
    beginRemoveRows({}, rows.first, rows.first + rows.count - 1);
    for (int row = rows.first; row < rows.first + rows.count; ++row) {
        index_.erase(rows_[row].shot.primary);
    }
    rows_.erase(rows_.begin() + rows.first, rows_.begin() + rows.first + rows.count);
    shiftSpans(std::next(span), -rows.count);
    spans_.erase(span);
    reindexFrom(rows.first);
    endRemoveRows();
}

void ShotModel::shiftSpans(Spans::iterator from, int delta) {
    if (delta == 0) {
        return;
    }
    for (; from != spans_.end(); ++from) {
        from->second.first += delta;
    }
}

void ShotModel::reportIfLoaded() {
    if (pending_ == 0 && walking_ == Walking::None) {
        emit loadingFinished();
    }
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
        return shotTooltip(row.shot, row.exif, row.hasSidecar(), shownName(row.shot));
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
    case HasSidecarRole:
        return row.hasSidecar();
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
    noteSidecarWritten(primary);
}

void ShotModel::noteSidecarWritten(const std::filesystem::path& primary) {
    const int index = rowOf(primary);
    if (index < 0) {
        return;
    }
    Row& row = rows_[index];
    const bool had = row.hasSidecar();
    row.sidecarExists = sidecarStampOf(primary) != 0;
    if (had != row.hasSidecar()) {
        changed(index, {HasSidecarRole, Qt::ToolTipRole});
    }
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
        const bool hadSidecar = row.hasSidecar();
        switch (result.kind) {
        case JobKind::Exif:
            row.exif = result.exif;
            changed(index, {Qt::ToolTipRole});
            break;
        case JobKind::Marks:
            row.sidecarStamp = result.stamp;
            if (!row.marksSet) {
                row.sidecarExists = result.stamp != 0;
            }
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
                row.sidecarExists = result.stamp != 0;
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
        if (hadSidecar != row.hasSidecar()) {
            changed(index, {HasSidecarRole, Qt::ToolTipRole});
        }
    }
    reportIfLoaded();
}

void ShotModel::reindexFrom(int from) {
    for (int i = from; i < static_cast<int>(rows_.size()); ++i) {
        index_.insert_or_assign(rows_[i].shot.primary, i);
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
