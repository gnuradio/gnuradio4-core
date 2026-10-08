#ifndef GNURADIO_SHARED_STATE_HPP
#define GNURADIO_SHARED_STATE_HPP

#include <atomic>
#include <cstddef>

#include <gnuradio-4.0/Export.hpp>

// The values below are defined once, in the library gnuradio-shared-state. Code from every shared object of a program
// reads and writes the same values. A per-thread value has one copy per thread for the whole program.

namespace gr {
class Sequence;
}

namespace gr::message {

// messages dropped because the destination ring was full -- process-global diagnostic counter
GNURADIO_EXPORT std::atomic<std::size_t>& droppedMessageCount() noexcept;

} // namespace gr::message

namespace gr::detail {

// The wake sequence of the consumer that runs on the calling thread. A publish on that thread leaves the sequence
// alone: the consumer is not waiting while it publishes.
GNURADIO_EXPORT const Sequence*& publishWakeExempt() noexcept;

// the message spans that Block::processScheduledMessages() handed to a block's handler on the calling thread. Handling
// a message is not work: the progress sequence moves only for samples.
GNURADIO_EXPORT std::size_t& handledMessageSpans() noexcept;

// the work() calls on the calling thread to a block that drains an asynchronous input. Such a block ends after a bound
// of calls in which nothing it waits on moved.
GNURADIO_EXPORT std::size_t& drainingCalls() noexcept;

} // namespace gr::detail

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

#endif // GNURADIO_SHARED_STATE_HPP
