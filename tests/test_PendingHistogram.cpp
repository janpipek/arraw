// The generation-keyed matcher for the two async histogram readbacks (ADR 0033,
// issue #51). The readbacks complete independently and possibly out of order;
// this pins the matching/staleness logic that guards against emitting a
// mismatched pair — the one failure mode the GPU smoke-run can't reliably catch.
#include "PendingHistogram.h"
#include <catch2/catch_test_macros.hpp>
#include <QImage>

using Kind = PendingHistogramPair::Kind;

namespace {

// Distinct, identifiable samples: size doubles as a tag.
QImage finalImg() {
    return QImage(4, 4, QImage::Format_RGBX32FPx4);
}

QImage curveImg() {
    return QImage(2, 2, QImage::Format_RGBA8888);
}

} // namespace

TEST_CASE("PendingHistogramPair emits once both halves of a generation arrive", "[histogram]") {
    PendingHistogramPair pending;
    pending.supersede(1);

    CHECK_FALSE(pending.offer(1, Kind::CurveInput, curveImg()).has_value());

    auto pair = pending.offer(1, Kind::Final, finalImg());
    REQUIRE(pair.has_value());
    CHECK(pair->finalSample.size() == QSize(4, 4));
    CHECK(pair->curveInput.size() == QSize(2, 2));
}

TEST_CASE("PendingHistogramPair drops a stale result and does not fill its slot", "[histogram]") {
    PendingHistogramPair pending;
    pending.supersede(2);

    // A late readback from generation 1 must be ignored entirely.
    CHECK_FALSE(pending.offer(1, Kind::Final, finalImg()).has_value());

    // Only generation 2's own pair completes — proving the stale Final didn't
    // pre-fill the final slot.
    CHECK_FALSE(pending.offer(2, Kind::CurveInput, curveImg()).has_value());
    CHECK(pending.offer(2, Kind::Final, finalImg()).has_value());
}

TEST_CASE("PendingHistogramPair discards a half-filled generation on supersede", "[histogram]") {
    PendingHistogramPair pending;
    pending.supersede(1);
    CHECK_FALSE(pending.offer(1, Kind::Final, finalImg()).has_value()); // final half of gen 1

    pending.supersede(2); // gen 1's stored final is now stale and dropped

    // The gen-1 final must not pair with a gen-2 curve-input.
    CHECK_FALSE(pending.offer(2, Kind::CurveInput, curveImg()).has_value());
    CHECK(pending.offer(2, Kind::Final, finalImg()).has_value());
}

TEST_CASE("PendingHistogramPair resets after emitting a pair", "[histogram]") {
    PendingHistogramPair pending;
    pending.supersede(1);
    CHECK_FALSE(pending.offer(1, Kind::CurveInput, curveImg()).has_value());
    REQUIRE(pending.offer(1, Kind::Final, finalImg()).has_value());

    // A duplicate completion for the same generation must not re-emit.
    CHECK_FALSE(pending.offer(1, Kind::Final, finalImg()).has_value());
}
