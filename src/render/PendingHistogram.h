#pragma once
#include <optional>
#include <QImage>

/**
 * Generation-keyed matcher for the two async histogram readbacks (docs/adr/0033).
 *
 * The curve-input and final samples are read back independently and complete in
 * separate frames, possibly out of order. Every render that enqueues a fresh
 * pair calls supersede() to advance the generation; each completed readback is
 * handed back through offer(), tagged with the generation it was requested
 * under. offer() drops a result whose generation is no longer current (its pair
 * was superseded mid-flight) and returns the matched pair only once *both*
 * halves of the current generation have arrived — guarding the one failure mode
 * the GPU smoke-run can't reliably catch: a mismatched histogram pair. Pure
 * value logic so it can be unit-tested without a GPU (issue #51).
 */
class PendingHistogramPair {
public:
    enum class Kind { Final, CurveInput };

    struct Pair {
        QImage finalSample;
        QImage curveInput;
    };

    // Begin accumulating a new generation, discarding any half-filled older one.
    void supersede(quint64 generation) {
        gen_ = generation;
        finalSample_.reset();
        curveInput_.reset();
    }

    quint64 generation() const { return gen_; }

    // Offer a completed readback tagged with the generation it was requested
    // under. Returns the pair once both halves of the current generation are in;
    // std::nullopt while still waiting, or if the result is stale (superseded).
    std::optional<Pair> offer(quint64 generation, Kind kind, const QImage& img) {
        if (generation != gen_)
            return std::nullopt; // superseded — drop rather than risk a mismatch
        (kind == Kind::Final ? finalSample_ : curveInput_) = img;
        if (!finalSample_ || !curveInput_)
            return std::nullopt;
        Pair pair{*finalSample_, *curveInput_};
        finalSample_.reset();
        curveInput_.reset();
        return pair;
    }

private:
    quint64 gen_ = 0;
    std::optional<QImage> finalSample_;
    std::optional<QImage> curveInput_;
};
