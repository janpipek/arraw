#pragma once

#include "RowBands.h"

#include <cstdint>

namespace arraw::test {

/// @brief A cap on the row bands that lasts for a scope and is lifted even when it throws.
class ScopedRowBandLimit {
public:
    /// @brief Sets the cap on the number of row bands.
    /// @param limit Most bands a pass may split into, 0 for no cap.
    explicit ScopedRowBandLimit(std::uint32_t limit) {
        detail::rowBandLimit = limit;
    }

    /// @brief Lifts the cap.
    ~ScopedRowBandLimit() {
        detail::rowBandLimit = 0;
    }

    ScopedRowBandLimit(const ScopedRowBandLimit&) = delete;
    ScopedRowBandLimit& operator=(const ScopedRowBandLimit&) = delete;
};

} // namespace arraw::test
