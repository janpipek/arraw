#include "Exiv2Support.h"

#include <exiv2/exiv2.hpp>

#include <mutex>

using namespace arraw;

namespace {

/// @brief Serialises exiv2's registration of XMP namespaces between threads.
std::mutex xmpMutex;

/// @brief Locks or unlocks ::xmpMutex for the XMP toolkit.
void lockXmp(void*, bool lock) {
    if (lock) {
        xmpMutex.lock();
    } else {
        xmpMutex.unlock();
    }
}

} // namespace

void exiv2support::prepare() {
    // A function-local static is initialised thread-safely, which is what
    // XmpParser::initialize needs from its caller. The log is muted because
    // exiv2 writes warnings to stderr by default, and a slightly malformed
    // maker note would otherwise print on every thumbnail.
    static const bool prepared = [] {
        Exiv2::LogMsg::setLevel(Exiv2::LogMsg::mute);
        Exiv2::XmpParser::initialize(lockXmp, nullptr);
        return true;
    }();
    (void)prepared;
}

std::string exiv2support::path(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
#else
    return path.string();
#endif
}
