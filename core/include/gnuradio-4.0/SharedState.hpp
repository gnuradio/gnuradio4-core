#ifndef GNURADIO_SHARED_STATE_HPP
#define GNURADIO_SHARED_STATE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <gnuradio-4.0/Export.hpp>

// The values below are defined once, in the library gnuradio-shared-state. Code from every shared object of a program
// reads and writes the same values. A per-thread value has one copy per thread for the whole program.

namespace gr {
class ComputeRegistry;
class Sequence;
class double_mapped_memory_resource;
} // namespace gr

namespace gr::message {

// messages dropped because the destination ring was full -- process-global diagnostic counter
GNURADIO_EXPORT std::atomic<std::size_t>& droppedMessageCount() noexcept;

} // namespace gr::message

namespace gr::detail {

// a plugin loader's refusal of a shared object: the reason it gave, and the warning it printed or an empty string
struct RefusedLibrary {
    std::string reason;
    std::string warning;
};

// Records that a plugin loader refused the shared object that the dlopen handle names. The caller leaves the object
// mapped. A later dlopen of its file then returns the same handle, and no other object takes that handle.
GNURADIO_EXPORT void recordRefusedLibrary(const void* handle, const RefusedLibrary& refusal);

// the refusal recorded for the shared object that the dlopen handle names, if a plugin loader refused it
GNURADIO_EXPORT std::optional<RefusedLibrary> refusedLibrary(const void* handle);

// The wake sequence of the consumer that runs on the calling thread. A publish on that thread leaves the sequence
// alone: the consumer is not waiting while it publishes.
GNURADIO_EXPORT const Sequence*& publishWakeExempt() noexcept;

// the message spans that Block::processScheduledMessages() handed to a block's handler on the calling thread. Handling
// a message is not work: the progress sequence moves only for samples.
GNURADIO_EXPORT std::size_t& handledMessageSpans() noexcept;

// the work() calls on the calling thread to a block that drains an asynchronous input. Such a block ends after a bound
// of calls in which nothing it waits on moved.
GNURADIO_EXPORT std::size_t& drainingCalls() noexcept;

// the registry that ComputeRegistry::instance() returns. A provider runs the code of the shared object that defines it,
// and that object must stay mapped while the provider is registered.
GNURADIO_EXPORT ComputeRegistry& computeRegistry();

// the resource that double_mapped_memory_resource::defaultAllocator() returns
GNURADIO_EXPORT double_mapped_memory_resource* defaultDoubleMappedResource();

// the number in the memory file name of the next double-mapped buffer
GNURADIO_EXPORT std::atomic<std::size_t>& doubleMappedBufferCount() noexcept;

// the unique_id of the next block whose type has the name typeName. Each type name counts from zero.
GNURADIO_EXPORT std::size_t nextBlockId(std::string_view typeName);

} // namespace gr::detail

namespace gr::profiling::detail {

// the number in the file name of the next Profiler that writes to a file without a given name
GNURADIO_EXPORT std::atomic<std::size_t>& traceFileCount() noexcept;

} // namespace gr::profiling::detail

namespace gr::scheduler {

// identifies the scheduler whose poolWorker() is running on this thread
GNURADIO_EXPORT const void*& activeSchedulerWorker() noexcept;

// the run generation of the worker that activeSchedulerWorker() names
GNURADIO_EXPORT std::size_t& activeWorkerGeneration() noexcept;

// identifies the scheduler whose worker is inside a work() call on this thread
GNURADIO_EXPORT const void*& workingScheduler() noexcept;

// identifies the scheduler whose deferred swap or restart this thread applies after its worker has left. The thread
// returns to its pool once the next run is dispatched.
GNURADIO_EXPORT const void*& applyingScheduler() noexcept;

// identifies the scheduler whose graph swap this thread holds, so the swap's own call to reset() does not wait for it
GNURADIO_EXPORT const void*& exchangingScheduler() noexcept;

} // namespace gr::scheduler

namespace gr::thread_pool {
class Manager;
}

namespace gr::thread_pool::detail {

// the thread pool manager that Manager::instance() returns, constructed with its default pools on the first call
GNURADIO_EXPORT Manager& threadPoolManager();

// the threads that the program's BasicThreadPool instances have started and not yet joined
GNURADIO_EXPORT std::atomic_size_t& globalThreadCount() noexcept;

// the number in the name of the next BasicThreadPool constructed without a name
GNURADIO_EXPORT std::atomic<std::uint64_t>& globalPoolId() noexcept;

// the id of the latest task that a BasicThreadPool queued
GNURADIO_EXPORT std::atomic<std::uint64_t>& taskID() noexcept;

} // namespace gr::thread_pool::detail

#endif // GNURADIO_SHARED_STATE_HPP
