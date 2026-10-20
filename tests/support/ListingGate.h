#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace arraw::test {

/// @brief Holds a folder walk before it reads one directory, until released.
///
/// Installed as the read hook of a ::arraw::app::FolderLister (through the watcher or the
/// strip), it records every directory the lister is about to read and blocks the lister's
/// thread on the one named, so a test can act at a known point of the walk. Declare it after
/// the watcher or the strip it is installed in: it releases the walk when it goes, and a watcher
/// that goes first would wait for its blocked thread for ever.
class ListingGate {
public:
    /// @brief Makes a gate that stops the walk before it reads a directory.
    /// @param at Directory to stop at, spelled as the walk spells it.
    explicit ListingGate(std::filesystem::path at) : state_(std::make_shared<State>()) {
        state_->at = std::move(at);
    }

    ListingGate(const ListingGate&) = delete;
    ListingGate& operator=(const ListingGate&) = delete;

    /// @brief Lets the walk go on.
    ~ListingGate() {
        release();
    }

    /// @brief Gives the hook to install.
    /// @return A function for ::arraw::app::FolderLister::setReadHook.
    [[nodiscard]] std::function<void(const std::filesystem::path&)> hook() const {
        return [state = state_](const std::filesystem::path& directory) {
            std::unique_lock lock(state->mutex);
            state->read.push_back(directory);
            if (directory != state->at) {
                return;
            }
            state->reached = true;
            state->wake.wait(lock, [&] { return state->open; });
        };
    }

    /// @brief Tells whether the walk is held at the directory (or was, and was released).
    [[nodiscard]] bool reached() const {
        return state_->reached.load();
    }

    /// @brief Lets the walk read the directory and go on.
    void release() const {
        {
            const std::scoped_lock lock(state_->mutex);
            state_->open = true;
        }
        state_->wake.notify_all();
    }

    /// @brief Lists the directories the walk was about to read, in order.
    [[nodiscard]] std::vector<std::filesystem::path> read() const {
        const std::scoped_lock lock(state_->mutex);
        return state_->read;
    }

private:
    /// @brief What the hook and the test share.
    struct State {
        std::filesystem::path at;
        std::mutex mutex;
        std::condition_variable wake;
        bool open = false;
        std::atomic<bool> reached{false};
        std::vector<std::filesystem::path> read;
    };

    std::shared_ptr<State> state_;
};

} // namespace arraw::test
