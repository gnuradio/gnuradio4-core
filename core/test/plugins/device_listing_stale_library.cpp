#include <memory>
#include <utility>
#include <vector>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/DeviceListing.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>

/**
 * @brief A shared object, not a plugin, that registers a device listing and a scheduler at plugin ABI version 1.
 *
 * A loader refuses it for the scheduler's version, and the refusal removes the listing.
 */
namespace {

using StaleScheduler = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>;

std::unique_ptr<gr::SchedulerModel> makeStaleScheduler(gr::property_map parameters) { return std::make_unique<gr::SchedulerWrapper<StaleScheduler>>(std::move(parameters)); }

std::vector<gr::DeviceUnit> fakeStaleUnits() { return {{"Fake stale unit", "fakestale:index=0"}}; }

const bool listingRegistered [[maybe_unused]]   = gr::globalDeviceListingRegistry().add({.family = "fakestale", .opens = false, .receive = true, .transmit = false, .listUnits = fakeStaleUnits});
const bool schedulerRegistered [[maybe_unused]] = gr::globalSchedulerRegistry().insert("test::stale_listing_scheduler", "", makeStaleScheduler, 1);

} // namespace
