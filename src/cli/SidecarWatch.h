#pragma once

#include <Diagnostics.h>

namespace arraw::cli {

/// @brief Log that passes everything on and notes whether a sidecar was unreadable.
class SidecarWatch final : public DiagnosticLog {
public:
    /// @brief Wraps the log everything is passed on to.
    /// @param next Log that receives every diagnostic; must outlive this one.
    explicit SidecarWatch(DiagnosticLog& next) : next_(next) {}

    void record(const Diagnostic& diagnostic) override {
        unreadable = unreadable || diagnostic.notice == Notice::SidecarUnreadable;
        next_.record(diagnostic);
    }

    /// @brief Whether a sidecar was reported unreadable.
    bool unreadable = false;

private:
    DiagnosticLog& next_;
};

} // namespace arraw::cli
