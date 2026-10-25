#pragma once

#include <Sidecar.h>
#include <Snapshots.h>

#include <cstddef>
#include <functional>

namespace arraw::detail {

/// @brief Largest serialized snapshot list a snapshot edit may leave; lowered only by tests.
inline std::size_t snapshotSubtreeLimit = maximumSnapshotSubtreeBytes;

/// @brief Time a writer waits for the sidecar lock, in milliseconds; lowered only by tests.
inline int sidecarLockTimeout = sidecarLockTimeoutMilliseconds;

/// @brief Age after which the sidecar lock is stale, in milliseconds; lowered only by tests.
inline int sidecarStaleLock = sidecarStaleLockMilliseconds;

/// @brief Function called by every writer right after it has the sidecar lock; set only by tests,
/// before any writer runs, to hold a writer inside its lock.
inline std::function<void()> sidecarLockHeldHook;

} // namespace arraw::detail
