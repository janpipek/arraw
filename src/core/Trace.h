#pragma once

#include <cstdio>
#include <cstdlib>
#include <QElapsedTimer>

// Opt-in timing trace for expensive operations. Enable by setting the ARRAW_TRACE
// environment variable; output goes to stderr as:
//
//     [trace] <label>             N ms
//
// Effectively free when disabled (a single cached environment lookup). Use
// trace::Scope to time a whole function/scope, or trace::Laps for a multi-stage
// operation where each stage's cost is wanted separately.
namespace trace {

inline bool enabled() {
    static const bool on = std::getenv("ARRAW_TRACE") != nullptr;
    return on;
}

inline void log(const char* label, qint64 ms) {
    std::fprintf(stderr, "[trace] %-24s %5lld ms\n", label, static_cast<long long>(ms));
    std::fflush(stderr);
}

// RAII: reports the wall time of the enclosing scope on destruction.
class Scope {
public:
    explicit Scope(const char* label)
        : label(label) {
        if (enabled())
            timer.start();
    }

    ~Scope() {
        if (enabled() && timer.isValid())
            log(label, timer.elapsed());
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const char* label;
    QElapsedTimer timer;
};

// Sequential lap timer: each lap() reports the time since the previous lap
// (or since construction) for multi-stage operations.
class Laps {
public:
    Laps() {
        if (enabled())
            timer.start();
    }

    void lap(const char* stage) {
        if (enabled() && timer.isValid())
            log(stage, timer.restart());
    }

private:
    QElapsedTimer timer;
};

} // namespace trace
