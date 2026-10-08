#include <mutex>
#include <unordered_map>

#include <gnuradio-4.0/ComputeDomain.hpp>
#include <gnuradio-4.0/SharedState.hpp>
#include <gnuradio-4.0/thread/thread_pool.hpp>

namespace gr::message {

std::atomic<std::size_t>& droppedMessageCount() noexcept {
    static std::atomic<std::size_t> nDropped{0UZ};
    return nDropped;
}

} // namespace gr::message

namespace gr::detail {

namespace {

struct RefusedLibraries {
    std::mutex                                      mutex;
    std::unordered_map<const void*, RefusedLibrary> refusals;
};

RefusedLibraries& refusedLibraries() {
    static RefusedLibraries libraries;
    return libraries;
}

} // namespace

void recordRefusedLibrary(const void* handle, const RefusedLibrary& refusal) {
    RefusedLibraries& libraries = refusedLibraries();
    std::scoped_lock  lock(libraries.mutex);
    libraries.refusals.insert_or_assign(handle, refusal);
}

std::optional<RefusedLibrary> refusedLibrary(const void* handle) {
    RefusedLibraries& libraries = refusedLibraries();
    std::scoped_lock  lock(libraries.mutex);
    if (const auto it = libraries.refusals.find(handle); it != libraries.refusals.end()) {
        return it->second;
    }
    return std::nullopt;
}

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

ComputeRegistry& computeRegistry() {
    static ComputeRegistry registry;
    return registry;
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

namespace gr::thread_pool::detail {

Manager& threadPoolManager() {
    static Manager manager;
    return manager;
}

std::atomic_size_t& globalThreadCount() noexcept {
    static std::atomic_size_t nThreads{0UZ};
    return nThreads;
}

std::atomic<std::uint64_t>& globalPoolId() noexcept {
    static std::atomic<std::uint64_t> poolId{0U};
    return poolId;
}

std::atomic<std::uint64_t>& taskID() noexcept {
    static std::atomic<std::uint64_t> id{0U};
    return id;
}

} // namespace gr::thread_pool::detail
