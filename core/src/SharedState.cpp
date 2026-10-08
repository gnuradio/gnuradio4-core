#include <gnuradio-4.0/SharedState.hpp>

namespace gr::message {

std::atomic<std::size_t>& droppedMessageCount() noexcept {
    static std::atomic<std::size_t> nDropped{0UZ};
    return nDropped;
}

} // namespace gr::message

namespace gr::detail {

const Sequence*& publishWakeExempt() noexcept {
    thread_local const Sequence* exempt = nullptr;
    return exempt;
}

std::size_t& handledMessageSpans() noexcept {
    thread_local std::size_t nHandled = 0UZ;
    return nHandled;
}

std::size_t& drainingCalls() noexcept {
    thread_local std::size_t nCalls = 0UZ;
    return nCalls;
}

} // namespace gr::detail

namespace gr::scheduler {

const void*& activeSchedulerWorker() noexcept {
    thread_local const void* scheduler = nullptr;
    return scheduler;
}

std::size_t& activeWorkerGeneration() noexcept {
    thread_local std::size_t generation = 0UZ;
    return generation;
}

const void*& workingScheduler() noexcept {
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
