#pragma once

#include <QtGlobal>

namespace arraw::app {

class QtMessageModel;

/// @brief Copies every message Qt handles into a table, for as long as it lives.
///
/// Installs a Qt message handler that hands each message on to the handler
/// before it, so it still reaches the terminal as it did. Messages in the
/// diagnostics' own category are left out of the table, which shows the
/// diagnostics structured elsewhere. One at a time.
class QtMessageCapture {
public:
    /// @brief Starts copying messages.
    /// @param model Table to copy them into; must outlive the capture.
    explicit QtMessageCapture(QtMessageModel& model);

    QtMessageCapture(const QtMessageCapture&) = delete;
    QtMessageCapture& operator=(const QtMessageCapture&) = delete;
    QtMessageCapture(QtMessageCapture&&) = delete;
    QtMessageCapture& operator=(QtMessageCapture&&) = delete;

    /// @brief Stops copying, and puts the handler before it back.
    ~QtMessageCapture();

private:
    QtMessageHandler previous_ = nullptr;
};

} // namespace arraw::app
