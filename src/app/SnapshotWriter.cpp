#include "SnapshotWriter.h"

#include "ShotModel.h"

#include <Diagnostics.h>

#include <QDebug>
#include <QString>

#include <algorithm>
#include <exception>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace arraw::app {

namespace {

/// @brief Tells whether a request is a thumbnail patch.
bool isPatch(const SnapshotWrite& write) {
    return std::holds_alternative<SnapshotThumbnailPatch>(write.change);
}

/// @brief Names a failure for the Qt log.
QString describe(const SnapshotWriteResult& result) {
    return QString::fromStdString(result.write.photo.string() + ": " + result.error);
}

} // namespace

SnapshotWriter::SnapshotWriter(std::function<void(SnapshotWriteResult)> onResult)
    : onResult_(std::move(onResult)), thread_([this] { run(); }) {}

SnapshotWriter::~SnapshotWriter() {
    {
        const std::scoped_lock lock(mutex_);
        delivering_ = false;
        stopping_ = true;
        // Derived data: made again the next time the photograph is opened.
        std::erase_if(queue_, [](const Request& request) { return isPatch(request.write); });
    }
    wake_.notify_all();
    thread_.join();
}

std::uint64_t SnapshotWriter::submit(SnapshotWrite write, std::uint64_t owner) {
    std::uint64_t ticket = 0;
    {
        const std::scoped_lock lock(mutex_);
        ticket = ++ticket_;
        // Patches of an earlier session are derived data its successor asks for again; they
        // must not hold up this request.
        std::erase_if(queue_, [owner](const Request& request) {
            return isPatch(request.write) && request.owner < owner;
        });
        queue_.push_back({ticket, owner, std::move(write)});
    }
    wake_.notify_one();
    return ticket;
}

std::size_t SnapshotWriter::pending() const {
    const std::scoped_lock lock(mutex_);
    return queue_.size() + running_;
}

void SnapshotWriter::setBeforeWriteHook(std::function<void()> hook) {
    const std::scoped_lock lock(mutex_);
    beforeWrite_ = std::move(hook);
}

std::optional<std::deque<SnapshotWriter::Request>> SnapshotWriter::take() {
    std::unique_lock lock(mutex_);
    wake_.wait(lock, [&] { return !queue_.empty() || stopping_; });
    // Nothing is left to do once stopping: the destructor dropped the patches, and nothing is
    // submitted after it began.
    if (queue_.empty()) {
        return std::nullopt;
    }
    std::deque<Request> run;
    run.push_back(std::move(queue_.front()));
    queue_.pop_front();
    if (isPatch(run.front().write)) {
        while (!queue_.empty() && run.size() < maximumBatch && isPatch(queue_.front().write) &&
               queue_.front().write.photo == run.front().write.photo) {
            run.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
    }
    running_ = run.size();
    return run;
}

void SnapshotWriter::carryOut(std::deque<Request> run) {
    std::vector<SnapshotWriteResult> results;
    for (Request& request : run) {
        SnapshotWriteResult result;
        result.ticket = request.ticket;
        result.owner = request.owner;
        result.write = std::move(request.write);
        results.push_back(std::move(result));
    }
    std::function<void()> hook;
    {
        const std::scoped_lock lock(mutex_);
        hook = beforeWrite_;
    }
    // Nothing may escape the thread, or the process terminates.
    try {
        if (hook) {
            hook();
        }
        if (isPatch(results.front().write)) {
            std::vector<SnapshotThumbnailPatch> patches;
            for (const SnapshotWriteResult& result : results) {
                patches.push_back(std::get<SnapshotThumbnailPatch>(result.write.change));
            }
            const auto& photo = results.front().write.photo;
            try {
                const std::vector<bool> written = writeSnapshotThumbnailPatches(photo, patches);
                for (std::size_t i = 0; i < results.size(); ++i) {
                    results[i].outcome.written = written[i];
                }
            } catch (const std::invalid_argument&) {
                if (patches.size() == 1) {
                    throw;
                }
                // One patch, or the sum, is over a limit: each alone may still fit.
                for (std::size_t i = 0; i < results.size(); ++i) {
                    try {
                        const std::span<const SnapshotThumbnailPatch> one(&patches[i], 1);
                        results[i].outcome.written =
                            writeSnapshotThumbnailPatches(photo, one).front();
                    } catch (const std::invalid_argument& error) {
                        results[i].failure = SnapshotWriteResult::Failure::Refused;
                        results[i].error = error.what();
                    }
                }
            }
        } else {
            results.front().outcome = writeSnapshotChange(results.front().write);
        }
    } catch (const SnapshotRevisionConflict& error) {
        SnapshotWriteResult& result = results.front();
        result.failure = SnapshotWriteResult::Failure::Conflict;
        result.error = error.what();
        try {
            CollectedDiagnostics log;
            if (const auto contents = readSidecar(result.write.photo, log)) {
                SnapshotUpdate fresh;
                fresh.collection = contents->snapshots;
                fresh.savedBase = contents->savedBase;
                fresh.unreadable = contents->unreadableSnapshots;
                result.fresh = std::move(fresh);
            }
        } catch (const std::exception&) {
            // The conflict is the error to report; the caller can read the sidecar itself.
        }
    } catch (const std::exception& error) {
        const auto failure = dynamic_cast<const SidecarLockTimeout*>(&error) != nullptr
                                 ? SnapshotWriteResult::Failure::LockTimeout
                                 : (dynamic_cast<const std::invalid_argument*>(&error) != nullptr
                                        ? SnapshotWriteResult::Failure::Refused
                                        : SnapshotWriteResult::Failure::Error);
        for (SnapshotWriteResult& result : results) {
            result.failure = failure;
            result.error = error.what();
        }
    } catch (...) {
        for (SnapshotWriteResult& result : results) {
            result.failure = SnapshotWriteResult::Failure::Error;
            result.error = "unknown error";
        }
    }

    // The stamp of the sidecar right after the commit, for the watcher to tell it from another
    // program's change.
    if (std::ranges::any_of(
            results, [](const SnapshotWriteResult& result) { return result.outcome.written; })) {
        try {
            const auto stamp = fileStamp(sidecarPath(results.front().write.photo));
            for (SnapshotWriteResult& result : results) {
                result.stamp = stamp;
            }
        } catch (const std::exception&) {
            // No stamp: the watcher will take the write for a change and the window rechecks.
        }
    }

    for (SnapshotWriteResult& result : results) {
        bool deliver = false;
        {
            const std::scoped_lock lock(mutex_);
            deliver = delivering_;
        }
        if (deliver) {
            try {
                onResult_(std::move(result));
            } catch (const std::exception& error) {
                qWarning().noquote() << "A snapshot result was not delivered:" << error.what();
            } catch (...) {
                qWarning() << "A snapshot result was not delivered.";
            }
        } else if (result.failure != SnapshotWriteResult::Failure::None) {
            qWarning().noquote() << "A snapshot change was not written:" << describe(result);
        }
        // Counted as owed until delivered, so that pending() of zero means nothing is in flight.
        const std::scoped_lock lock(mutex_);
        if (running_ > 0) {
            --running_;
        }
    }
}

void SnapshotWriter::run() {
    while (auto run = take()) {
        carryOut(std::move(*run));
    }
}

} // namespace arraw::app
