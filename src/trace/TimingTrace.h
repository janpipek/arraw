#pragma once

#include <QLoggingCategory>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace arraw::detail {

Q_DECLARE_LOGGING_CATEGORY(timingLog)

/// @brief Opt-in elapsed-time trace with nesting and worker-request correlation.
///
/// Enabled through QT_LOGGING_RULES="arraw.timing.debug=true". Measures host time,
/// including GPU submission waits, rather than GPU hardware timestamps. Disabled
/// spans do not read the clock or allocate their label. Children inherit the
/// request identifier on their thread; span identifiers are process-wide.
class TimingSpan {
public:
    /// @brief Starts a span when timing output is enabled.
    /// @param label Operation name, copied only when tracing is enabled.
    /// @param request Preview or export request identifier; zero inherits its parent's.
    /// @param detail Additional context, such as the GPU pass name.
    explicit TimingSpan(std::string_view label, std::uint64_t request = 0,
                        std::string_view detail = {});

    TimingSpan(const TimingSpan&) = delete;
    TimingSpan& operator=(const TimingSpan&) = delete;
    TimingSpan(TimingSpan&&) = delete;
    TimingSpan& operator=(TimingSpan&&) = delete;

    /// @brief Writes the end time and restores the enclosing span, including during unwinding.
    ~TimingSpan();

    /// @brief Writes contextual information for an active span.
    void note(std::string_view detail) const;

private:
    /// @brief Writes one event with its monotonic process timestamp.
    void write(std::string_view event, std::string_view detail = {}) const;

    std::uint64_t id_ = 0;
    std::uint64_t parentId_ = 0;
    std::uint64_t request_ = 0;
    TimingSpan* parent_ = nullptr;
    std::string label_;
    std::chrono::steady_clock::time_point started_;
    int exceptions_ = 0;
};

} // namespace arraw::detail
