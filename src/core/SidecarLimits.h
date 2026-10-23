#pragma once

#include <Sidecar.h>
#include <Snapshots.h>

#include <cstddef>

namespace arraw::detail {

/// @brief Largest serialized snapshot list a snapshot edit may leave; lowered only by tests.
inline std::size_t snapshotSubtreeLimit = maximumSnapshotSubtreeBytes;

/// @brief Time a writer waits for the sidecar lock, in milliseconds; lowered only by tests.
inline int sidecarLockTimeout = sidecarLockTimeoutMilliseconds;

} // namespace arraw::detail
