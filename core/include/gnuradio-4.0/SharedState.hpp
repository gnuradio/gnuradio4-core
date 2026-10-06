#ifndef GNURADIO_SHARED_STATE_HPP
#define GNURADIO_SHARED_STATE_HPP

#include <gnuradio-4.0/Export.hpp>

// The values below are defined once, in the library gnuradio-shared-state. Code from every shared object of a program
// reads and writes the same values. A per-thread value has one copy per thread for the whole program.

namespace gr::scheduler {

// identifies the scheduler whose poolWorker() is running on this thread
GNURADIO_EXPORT const void*& activeSchedulerWorker() noexcept;

// identifies the scheduler whose deferred swap or restart this thread applies after its worker has left. The thread
// returns to its pool once the next run is dispatched.
GNURADIO_EXPORT const void*& applyingScheduler() noexcept;

// identifies the scheduler whose graph swap this thread holds, so the swap's own call to reset() does not wait for it
GNURADIO_EXPORT const void*& exchangingScheduler() noexcept;

} // namespace gr::scheduler

#endif // GNURADIO_SHARED_STATE_HPP
