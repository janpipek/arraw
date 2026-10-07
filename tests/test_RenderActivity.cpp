#include "RenderActivity.h"
#include "RenderIndicator.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <vector>

using namespace arraw;
using namespace arraw::app;
using namespace std::chrono_literals;

/// When a render in progress shows itself, and what it shows (ADR 042): nothing for a
/// fast one, a bar for a slow one, held for a moment, with times set by the test.

namespace {

using Clock = RenderActivity::Clock;

/// Start of the test's own time.
const Clock::time_point origin{};

[[nodiscard]] Clock::time_point at(std::chrono::milliseconds offset) {
    return origin + offset;
}

} // namespace

TEST_CASE("A render that finishes within the delay is never shown", "[app][progress]") {
    RenderActivity activity;
    activity.begin(at(0ms));
    CHECK_FALSE(activity.poll(at(0ms)).visible);
    activity.report(at(100ms), 0.5, ProgressStep::Denoise);
    CHECK_FALSE(activity.poll(at(249ms)).visible);
    activity.finish(at(249ms));
    CHECK_FALSE(activity.poll(at(249ms)).visible);
    CHECK_FALSE(activity.poll(at(2000ms)).visible);
    CHECK_FALSE(activity.busy());
    CHECK_FALSE(activity.nextChange().has_value());
}

TEST_CASE("A render that outlasts the delay is shown from the moment it ran out",
          "[app][progress]") {
    RenderActivity activity;
    activity.begin(at(0ms));
    REQUIRE(activity.nextChange() == at(renderActivityDelay));
    CHECK(activity.poll(at(250ms)).visible);
    // However late the poll: the time it counts from is the delay's end.
    RenderActivity late;
    late.begin(at(0ms));
    CHECK(late.poll(at(900ms)).visible);
    // Held from 250 ms, not from the late look: long past, so gone on a cancellation.
    late.finish(at(900ms), false);
    CHECK_FALSE(late.poll(at(900ms)).visible);
}

TEST_CASE("A shown render stays for the hold after it finished", "[app][progress]") {
    RenderActivity activity;
    activity.begin(at(0ms));
    REQUIRE(activity.poll(at(250ms)).visible);
    // Finished at 260 ms, 10 ms after it showed: held to 550 ms.
    activity.finish(at(260ms));
    CHECK(activity.poll(at(260ms)).visible);
    CHECK(activity.poll(at(549ms)).visible);
    CHECK(activity.nextChange() == at(250ms + renderActivityHold));
    CHECK_FALSE(activity.poll(at(550ms)).visible);
    CHECK_FALSE(activity.nextChange().has_value());
}

TEST_CASE("A render shown for longer than the hold shows its end, then goes", "[app][progress]") {
    RenderActivity activity;
    activity.begin(at(0ms));
    REQUIRE(activity.poll(at(1000ms)).visible);
    activity.finish(at(1000ms));
    const auto filled = activity.poll(at(1000ms));
    CHECK(filled.visible);
    CHECK(filled.fraction == 1.0);
    CHECK(activity.nextChange() == at(1000ms + renderActivityFilled));
    CHECK(activity.poll(at(1149ms)).visible);
    CHECK_FALSE(activity.poll(at(1150ms)).visible);
}

TEST_CASE("A render shown for longer than the hold goes at once when cancelled",
          "[app][progress]") {
    RenderActivity activity;
    activity.begin(at(0ms));
    REQUIRE(activity.poll(at(1000ms)).visible);
    activity.finish(at(1000ms), false);
    CHECK_FALSE(activity.poll(at(1000ms)).visible);
}

TEST_CASE("Reports give a determinate bar and the step", "[app][progress]") {
    RenderActivity activity;
    activity.begin(at(0ms));
    activity.report(at(10ms), 0.1, ProgressStep::Decode);
    activity.report(at(300ms), 0.4, ProgressStep::Denoise);
    const auto display = activity.poll(at(300ms));
    REQUIRE(display.visible);
    REQUIRE(display.fraction.has_value());
    CHECK_THAT(*display.fraction, Catch::Matchers::WithinAbs(0.4, 1e-12));
    CHECK(display.step == ProgressStep::Denoise);

    activity.report(at(350ms), 7.0, ProgressStep::Effects);
    CHECK(*activity.poll(at(350ms)).fraction == 1.0);
}

TEST_CASE("Without a fraction the bar is busy", "[app][progress]") {
    RenderActivity activity;
    activity.begin(at(0ms));
    const auto first = activity.poll(at(250ms));
    REQUIRE(first.visible);
    CHECK_FALSE(first.fraction.has_value());
}

TEST_CASE("A newer render keeps the busy period and the bar until it reports", "[app][progress]") {
    RenderActivity activity;
    activity.begin(at(0ms));
    activity.report(at(100ms), 0.9, ProgressStep::Effects);
    REQUIRE(activity.poll(at(400ms)).visible);
    // The edit that superseded it: the delay is not owed again, and no busy bar in between.
    activity.begin(at(410ms));
    const auto kept = activity.poll(at(410ms));
    CHECK(kept.visible);
    CHECK(kept.fraction == 0.9);
    CHECK(kept.step == ProgressStep::Effects);
    // Its own first report replaces it, lower or not.
    activity.report(at(415ms), 0.2, ProgressStep::Pointwise);
    const auto newer = activity.poll(at(415ms));
    CHECK(newer.fraction == 0.2);
    CHECK(newer.step == ProgressStep::Pointwise);
}

TEST_CASE("A new busy period starts with no fraction", "[app][progress]") {
    RenderActivity activity;
    activity.begin(at(0ms));
    activity.report(at(100ms), 0.9, ProgressStep::Effects);
    REQUIRE(activity.poll(at(400ms)).visible);
    activity.finish(at(400ms));
    activity.begin(at(450ms));
    const auto display = activity.poll(at(450ms));
    CHECK(display.visible);
    CHECK_FALSE(display.fraction.has_value());
    CHECK(display.step == ProgressStep::Pointwise);
}

TEST_CASE("A render asked for during the hold keeps the bar up", "[app][progress]") {
    RenderActivity activity;
    activity.begin(at(0ms));
    REQUIRE(activity.poll(at(250ms)).visible);
    activity.finish(at(300ms));
    activity.begin(at(400ms));
    CHECK(activity.poll(at(700ms)).visible);
    activity.finish(at(700ms), false);
    CHECK_FALSE(activity.poll(at(700ms)).visible);
}

TEST_CASE("A cancelled render hides without filling the bar", "[app][progress]") {
    RenderActivity activity;
    activity.begin(at(0ms));
    activity.report(at(100ms), 0.3, ProgressStep::Denoise);
    REQUIRE(activity.poll(at(260ms)).visible);
    activity.finish(at(270ms), false);
    const auto held = activity.poll(at(300ms));
    CHECK(held.visible);
    CHECK(*held.fraction == 0.3);
    CHECK_FALSE(activity.poll(at(550ms)).visible);
}

TEST_CASE("A completed render fills the bar for the hold", "[app][progress]") {
    RenderActivity activity;
    activity.begin(at(0ms));
    activity.report(at(100ms), 0.7, ProgressStep::Effects);
    REQUIRE(activity.poll(at(260ms)).visible);
    activity.finish(at(270ms));
    CHECK(*activity.poll(at(300ms)).fraction == 1.0);
}

TEST_CASE("A report with no render in progress is ignored", "[app][progress]") {
    RenderActivity activity;
    activity.report(at(0ms), 0.5, ProgressStep::Denoise);
    CHECK_FALSE(activity.poll(at(500ms)).visible);
    activity.begin(at(500ms));
    activity.finish(at(510ms));
    activity.report(at(520ms), 0.5, ProgressStep::Denoise);
    CHECK_FALSE(activity.poll(at(2000ms)).visible);
}

TEST_CASE("Steps are worded in British English", "[app][progress]") {
    CHECK(renderStepText(ProgressStep::Decode) == QString::fromUtf8("Decoding…"));
    CHECK(renderStepText(ProgressStep::Denoise) == QString::fromUtf8("Reducing noise…"));
    CHECK(renderStepText(ProgressStep::Pointwise) == QString::fromUtf8("Developing…"));
    CHECK(renderStepText(ProgressStep::Context).contains("Analysing"));
}

TEST_CASE("The indicator announces each change of what is shown", "[app][progress]") {
    Clock::time_point now = origin;
    RenderIndicator indicator([&now] { return now; });
    std::vector<RenderActivity::Display> seen;
    QObject::connect(&indicator, &RenderIndicator::changed, &indicator,
                     [&seen](const RenderActivity::Display& display) { seen.push_back(display); });

    indicator.begin();
    now = at(100ms);
    indicator.poll();
    CHECK(seen.empty());

    now = at(250ms);
    indicator.poll();
    REQUIRE(seen.size() == 1);
    CHECK(seen.back().visible);
    CHECK_FALSE(seen.back().fraction.has_value());

    now = at(300ms);
    indicator.report(0.5, ProgressStep::Denoise);
    REQUIRE(seen.size() == 2);
    CHECK(*seen.back().fraction == 0.5);
    CHECK(seen.back().step == ProgressStep::Denoise);

    indicator.report(0.5, ProgressStep::Denoise);
    CHECK(seen.size() == 2); // Nothing changed.

    now = at(600ms);
    indicator.finish();
    // Shown for more than the hold already: the full bar for a moment, then nothing.
    CHECK(seen.back().fraction == 1.0);
    now = at(600ms) + renderActivityFilled;
    indicator.poll();
    CHECK_FALSE(seen.back().visible);
    CHECK_FALSE(indicator.display().visible);
}

TEST_CASE("The indicator hides a held bar when the hold ends", "[app][progress]") {
    Clock::time_point now = origin;
    RenderIndicator indicator([&now] { return now; });
    int hidden = 0;
    QObject::connect(
        &indicator, &RenderIndicator::changed, &indicator,
        [&hidden](const RenderActivity::Display& display) { hidden += display.visible ? 0 : 1; });
    indicator.begin();
    now = at(250ms);
    indicator.poll();
    now = at(260ms);
    indicator.finish();
    CHECK(indicator.display().visible);
    CHECK(hidden == 0);
    now = at(549ms);
    indicator.poll();
    CHECK(indicator.display().visible);
    now = at(550ms);
    indicator.poll();
    CHECK_FALSE(indicator.display().visible);
    CHECK(hidden == 1);
}
