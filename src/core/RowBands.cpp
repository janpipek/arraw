#include "RowBands.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>

#include <pthread/qos.h>
#endif

namespace arraw::detail {

int callerThreadPriority() noexcept {
#if defined(_WIN32)
    const int priority = GetThreadPriority(GetCurrentThread());
    return priority == THREAD_PRIORITY_ERROR_RETURN ? THREAD_PRIORITY_NORMAL : priority;
#elif defined(__APPLE__)
    return static_cast<int>(qos_class_self());
#else
    return 0;
#endif
}

void adoptThreadPriority([[maybe_unused]] int priority) noexcept {
#if defined(_WIN32)
    if (priority != GetThreadPriority(GetCurrentThread())) {
        SetThreadPriority(GetCurrentThread(), priority);
    }
#elif defined(__APPLE__)
    const auto qos = static_cast<qos_class_t>(priority);
    if (qos != qos_class_self() && qos != QOS_CLASS_UNSPECIFIED) {
        pthread_set_qos_class_self_np(qos, 0);
    }
#endif
}

} // namespace arraw::detail
