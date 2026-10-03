#include "TimingTrace.h"

#include <QString>

#include <catch2/catch_test_macros.hpp>

#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace arraw;

namespace {

std::mutex messagesMutex;
std::vector<QString> messages;

/// @brief Captures only timing events while the trace tests run.
void capture(QtMsgType, const QMessageLogContext& context, const QString& message) {
    if (context.category && std::string_view(context.category) == "arraw.timing") {
        const std::scoped_lock lock(messagesMutex);
        messages.push_back(message);
    }
}

/// @brief Temporary timing capture that restores the process's logging configuration.
class Capture {
public:
    explicit Capture(bool enabled)
        : category_(const_cast<QLoggingCategory&>(detail::timingLog())),
          wasEnabled_(category_.isDebugEnabled()), previous_(qInstallMessageHandler(capture)) {
        messages.clear();
        category_.setEnabled(QtDebugMsg, enabled);
    }
    ~Capture() {
        category_.setEnabled(QtDebugMsg, wasEnabled_);
        qInstallMessageHandler(previous_);
    }

private:
    QLoggingCategory& category_;
    bool wasEnabled_;
    QtMessageHandler previous_;
};

/// @brief Reads a space-delimited correlation field from a timing event.
QString field(const QString& message, const QString& key) {
    const QString prefix = key + '=';
    const auto start = message.indexOf(prefix);
    REQUIRE(start >= 0);
    return message.mid(start + prefix.size()).section(' ', 0, 0);
}

} // namespace

TEST_CASE("Disabled timing emits no output", "[timing]") {
    const Capture capture(false);
    {
        const detail::TimingSpan span("test.disabled", 123);
        span.note("ignored");
    }
    REQUIRE(messages.empty());
}

TEST_CASE("Timing spans inherit request IDs and preserve nesting", "[timing]") {
    const Capture capture(true);
    {
        const detail::TimingSpan parent("test.parent", 123);
        {
            const detail::TimingSpan child("test.child");
            child.note("context");
        }
        parent.note("after child");
    }
    REQUIRE(messages.size() == 6);
    REQUIRE(field(messages[0], "event") == "begin");
    REQUIRE(field(messages[5], "event") == "end");
    REQUIRE(field(messages[1], "request") == "123");
    REQUIRE(field(messages[1], "parent") == field(messages[0], "span"));
    REQUIRE(field(messages[3], "span") == field(messages[1], "span"));
    REQUIRE(field(messages[4], "span") == field(messages[0], "span"));
    for (const auto& message : messages) {
        const auto timestamp = field(message, "t");
        const auto elapsed = field(message, "elapsed");
        REQUIRE(timestamp.endsWith("ms"));
        REQUIRE(elapsed.endsWith("ms"));
        REQUIRE(timestamp.chopped(2).toDouble() >= 0.0);
        REQUIRE(elapsed.chopped(2).toDouble() >= 0.0);
    }
}

TEST_CASE("Unwinding closes a timing span and restores its parent", "[timing]") {
    const Capture capture(true);
    {
        const detail::TimingSpan parent("test.parent", 42);
        try {
            const detail::TimingSpan child("test.throw");
            throw std::runtime_error("expected");
        } catch (const std::runtime_error&) {
            const detail::TimingSpan sibling("test.sibling");
        }
    }
    REQUIRE(messages.size() == 6);
    REQUIRE(messages[2].contains("detail=exception"));
    REQUIRE(field(messages[3], "parent") == field(messages[0], "span"));
    REQUIRE(field(messages[3], "request") == "42");
}

TEST_CASE("Workers have independent timing parents and unique span IDs", "[timing]") {
    const Capture capture(true);
    {
        const detail::TimingSpan parent("test.main", 99);
        std::jthread worker([] { const detail::TimingSpan child("test.worker", 7); });
        worker.join();
    }
    REQUIRE(messages.size() == 4);
    REQUIRE(field(messages[1], "parent") == "0");
    REQUIRE(field(messages[1], "request") == "7");
    REQUIRE(field(messages[0], "span") != field(messages[1], "span"));
}
