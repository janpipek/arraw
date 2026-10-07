#include "TimingTrace.h"

#include <QDebug>
#include <QString>

#include <atomic>
#include <exception>

namespace arraw::detail {

Q_LOGGING_CATEGORY(timingLog, "arraw.timing", QtWarningMsg)

namespace {

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::duration<double, std::milli>;

/// @brief Common monotonic origin for every worker's timestamps.
const auto origin = Clock::now();
std::atomic<std::uint64_t> nextId{0};
thread_local TimingSpan* current = nullptr;

} // namespace

TimingSpan::TimingSpan(std::string_view label, std::uint64_t request, std::string_view detail) {
    if (!timingLog().isDebugEnabled()) {
        return;
    }
    label_ = label;
    parent_ = current;
    parentId_ = parent_ ? parent_->id_ : 0;
    request_ = request != 0 ? request : parent_ ? parent_->request_ : 0;
    id_ = nextId.fetch_add(1, std::memory_order_relaxed) + 1;
    exceptions_ = std::uncaught_exceptions();
    started_ = Clock::now();
    write("begin", detail);
    current = this;
}

TimingSpan::~TimingSpan() {
    if (id_ == 0) {
        return;
    }
    current = parent_;
    write("end", std::uncaught_exceptions() > exceptions_ ? "exception" : "");
}

void TimingSpan::note(std::string_view detail) const {
    if (id_ != 0) {
        write("note", detail);
    }
}

void TimingSpan::write(std::string_view event, std::string_view detail) const {
    const auto now = Clock::now();
    qCDebug(timingLog).noquote().nospace()
        << "t=" << QString::number(Milliseconds(now - origin).count(), 'f', 3) << "ms span=" << id_
        << " parent=" << parentId_ << " request=" << request_
        << " stage=" << QString::fromStdString(label_) << " event=" << QString::fromUtf8(event)
        << " elapsed=" << QString::number(Milliseconds(now - started_).count(), 'f', 3) << "ms"
        << (detail.empty() ? "" : " detail=") << QString::fromUtf8(detail);
}

} // namespace arraw::detail
