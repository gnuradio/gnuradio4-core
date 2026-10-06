#ifndef GNURADIO_SHARED_STATE_HPP
#define GNURADIO_SHARED_STATE_HPP

#include <atomic>
#include <cstddef>

#include <gnuradio-4.0/Export.hpp>

// The values below are defined once, in the library gnuradio-shared-state. Code from every shared object of a program
// reads and writes the same values. A per-thread value has one copy per thread for the whole program.

namespace gr::message {

// messages dropped because the destination ring was full -- process-global diagnostic counter
GNURADIO_EXPORT std::atomic<std::size_t>& droppedMessageCount() noexcept;

} // namespace gr::message

namespace gr::scheduler {

// identifies the scheduler whose poolWorker() is running on this thread
GNURADIO_EXPORT const void*& activeSchedulerWorker() noexcept;

// the run generation of the worker that activeSchedulerWorker() names
GNURADIO_EXPORT std::size_t& activeWorkerGeneration() noexcept;

// identifies the scheduler whose deferred swap or restart this thread applies after its worker has left. The thread
// returns to its pool once the next run is dispatched.
GNURADIO_EXPORT const void*& applyingScheduler() noexcept;

// identifies the scheduler whose graph swap this thread holds, so the swap's own call to reset() does not wait for it
GNURADIO_EXPORT const void*& exchangingScheduler() noexcept;

} // namespace gr::scheduler

#endif // GNURADIO_SHARED_STATE_HPP
