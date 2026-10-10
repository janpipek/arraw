#pragma once

#include "TimingTrace.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <algorithm>
#include <string>

namespace arraw::test {

/// Longest gap between two turns of the event loop while a probe runs.
class StallProbe {
public:
    StallProbe() {
        timer_.setInterval(1);
        QObject::connect(&timer_, &QTimer::timeout, [this] {
            const qint64 now = clock_.nsecsElapsed();
            if (now - last_ > 12'000'000) {
                const detail::TimingSpan stall("bench.stall");
                stall.note(std::to_string((now - last_) / 1000000) + " ms");
            }
            longest_ = std::max(longest_, now - last_);
            last_ = now;
        });
    }

    void start() {
        longest_ = 0;
        clock_.start();
        last_ = 0;
        timer_.start();
    }

    /// Stops the probe's timer; read longestMs() before, as the open gap keeps growing after.
    void stop() {
        timer_.stop();
    }

    /// @brief Longest gap so far, including the one still open, in milliseconds.
    [[nodiscard]] double longestMs() const {
        return static_cast<double>(std::max(longest_, clock_.nsecsElapsed() - last_)) / 1e6;
    }

private:
    QTimer timer_;
    QElapsedTimer clock_;
    qint64 last_ = 0;
    qint64 longest_ = 0;
};

} // namespace arraw::test
