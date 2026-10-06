#include <gnuradio-4.0/SharedState.hpp>

namespace gr::scheduler {

const void*& activeSchedulerWorker() noexcept {
    thread_local const void* scheduler = nullptr;
    return scheduler;
}

const void*& applyingScheduler() noexcept {
    thread_local const void* scheduler = nullptr;
    return scheduler;
}

const void*& exchangingScheduler() noexcept {
    thread_local const void* scheduler = nullptr;
    return scheduler;
}

} // namespace gr::scheduler
