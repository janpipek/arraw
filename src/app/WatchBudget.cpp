#include "WatchBudget.h"

#include <Shot.h>

#include <algorithm>
#include <iterator>
#include <utility>

namespace arraw::app {

namespace fs = std::filesystem;

namespace {} // namespace

bool WatchBudget::Key::operator<(const Key& other) const {
    if (depth != other.depth) {
        return depth < other.depth;
    }
    return folderPrecedes(path, other.path);
}

WatchBudget::WatchBudget(int cap) : cap_(std::max(cap, 0)) {}

WatchBudget::Grant WatchBudget::add(const fs::path& path, int depth) {
    if (const auto known = depths_.find(path); known != depths_.end()) {
        return {.watch = watched_.contains(Key{known->second, path}), .evict = std::nullopt};
    }
    depths_.emplace(path, depth);
    Key key{depth, path};
    if (std::cmp_less(watched_.size(), cap_)) {
        watched_.insert(std::move(key));
        return {.watch = true, .evict = std::nullopt};
    }
    if (watched_.empty() || !(key < *watched_.rbegin())) {
        waiting_.insert(std::move(key));
        return {};
    }
    // Shallower than the last one watched: it gives up its place.
    auto last = std::prev(watched_.end());
    Grant grant{.watch = true, .evict = last->path};
    waiting_.insert(watched_.extract(last));
    watched_.insert(std::move(key));
    return grant;
}

std::vector<fs::path> WatchBudget::remove(std::span<const fs::path> paths) {
    for (const fs::path& path : paths) {
        const auto known = depths_.find(path);
        if (known == depths_.end()) {
            continue;
        }
        const Key key{known->second, path};
        if (watched_.erase(key) == 0) {
            waiting_.erase(key);
        }
        depths_.erase(known);
    }
    return promote();
}

std::optional<fs::path> WatchBudget::remove(const fs::path& path) {
    auto promoted = remove(std::span<const fs::path>(&path, 1));
    if (promoted.empty()) {
        return std::nullopt;
    }
    return std::move(promoted.front());
}

void WatchBudget::refused(const fs::path& path) {
    const auto known = depths_.find(path);
    if (known == depths_.end()) {
        return;
    }
    auto at = watched_.find(Key{known->second, path});
    if (at == watched_.end()) {
        return;
    }
    waiting_.insert(watched_.extract(at));
    // The system took this many and no more.
    cap_ = static_cast<int>(watched_.size());
}

WatchBudget::Moves WatchBudget::setCap(int cap) {
    cap_ = std::max(cap, 0);
    Moves moves;
    moves.evicted = shrink();
    moves.promoted = promote();
    return moves;
}

void WatchBudget::reset(int cap) {
    cap_ = std::max(cap, 0);
    watched_.clear();
    waiting_.clear();
    depths_.clear();
}

bool WatchBudget::isWatched(const fs::path& path) const {
    const auto known = depths_.find(path);
    return known != depths_.end() && watched_.contains(Key{known->second, path});
}

std::vector<fs::path> WatchBudget::promote() {
    std::vector<fs::path> promoted;
    while (std::cmp_less(watched_.size(), cap_) && !waiting_.empty()) {
        auto first = waiting_.begin();
        promoted.push_back(first->path);
        watched_.insert(waiting_.extract(first));
    }
    return promoted;
}

std::vector<fs::path> WatchBudget::shrink() {
    std::vector<fs::path> evicted;
    while (std::cmp_greater(watched_.size(), cap_)) {
        auto last = std::prev(watched_.end());
        evicted.push_back(last->path);
        waiting_.insert(watched_.extract(last));
    }
    return evicted;
}

} // namespace arraw::app
