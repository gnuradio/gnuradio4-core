#include <memory>
#include <utility>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>

/**
 * @brief A shared object that registers a scheduler but is not a plugin.
 *
 * Built twice from this file. As it is, its registration records the plugin ABI version this core implements. With
 * `SCHEDULER_LIBRARY_ABI_VERSION` defined, it records that version instead, as a library left installed from a core
 * of another version does, and a loader has to refuse it before anything calls the scheduler.
 */
namespace gr::testing {

using LibraryScheduler = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>;

std::unique_ptr<gr::SchedulerModel> makeLibraryScheduler(gr::property_map parameters) { return std::make_unique<gr::SchedulerWrapper<LibraryScheduler>>(std::move(parameters)); }

#ifdef SCHEDULER_LIBRARY_ABI_VERSION
const bool registered [[maybe_unused]] = gr::globalSchedulerRegistry().insert("test::library_scheduler_v1", "", makeLibraryScheduler, SCHEDULER_LIBRARY_ABI_VERSION);
#else
const bool registered [[maybe_unused]] = gr::globalSchedulerRegistry().insert("test::library_scheduler", "", makeLibraryScheduler);
#endif

} // namespace gr::testing
