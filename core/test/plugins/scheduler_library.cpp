#include <memory>
#include <string>
#include <utility>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>

/**
 * @brief A shared object that registers a scheduler but is not a plugin.
 *
 * Built three times from this file. As it is, its registration records the plugin ABI version this core implements.
 * With `SCHEDULER_LIBRARY_ABI_VERSION` defined, it records that version instead, as a library left installed from a
 * core of another version does. With `SCHEDULER_LIBRARY_UNVERSIONED` defined, its entry carries no version, as the
 * entry of a library built against a registry that records none does. A loader has to refuse the last two before
 * anything calls the scheduler.
 */
namespace gr::testing {

using LibraryScheduler = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>;

std::unique_ptr<gr::SchedulerModel> makeLibraryScheduler(gr::property_map parameters) { return std::make_unique<gr::SchedulerWrapper<LibraryScheduler>>(std::move(parameters)); }

#if defined(SCHEDULER_LIBRARY_ABI_VERSION)
const bool registered [[maybe_unused]] = gr::globalSchedulerRegistry().insert("test::library_scheduler_v1", "", makeLibraryScheduler, SCHEDULER_LIBRARY_ABI_VERSION);
#elif defined(SCHEDULER_LIBRARY_UNVERSIONED)
// a registry without versions takes the entry and counts the registration, and holds no version for it
const bool registered [[maybe_unused]] = [] {
    const std::string  key      = "test::library_scheduler_unversioned";
    SchedulerRegistry& registry = gr::globalSchedulerRegistry();
    registry.insert(key, "", makeLibraryScheduler);
    SchedulerRegistry::Entries entries = registry.takeEntries();
    entries.abiVersions.erase(key);
    registry.restoreEntries(std::move(entries), false);
    return true;
}();
#else
const bool registered [[maybe_unused]] = gr::globalSchedulerRegistry().insert("test::library_scheduler", "", makeLibraryScheduler);
#endif

} // namespace gr::testing
