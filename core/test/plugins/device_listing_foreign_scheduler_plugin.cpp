#include <memory>
#include <vector>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/DeviceListing.hpp>
#include <gnuradio-4.0/Plugin.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>

/// A plugin at the plugin ABI version this core implements whose load registers a device listing and a scheduler at
/// plugin ABI version 1.
namespace {

std::unique_ptr<gr::SchedulerModel> makeForeignScheduler(gr::property_map /*parameters*/) { return nullptr; }

std::vector<gr::DeviceUnit> fakeForeignSchedulerUnits() { return {{"Fake foreign scheduler unit", "fakeforeignscheduler:index=0"}}; }

const bool listingRegistered [[maybe_unused]]   = gr::globalDeviceListingRegistry().add({.family = "fakeforeignscheduler", .opens = false, .receive = true, .transmit = false, .listUnits = fakeForeignSchedulerUnits});
const bool schedulerRegistered [[maybe_unused]] = gr::globalSchedulerRegistry().insert("test::listing_plugin_foreign_scheduler", "", makeForeignScheduler, 1);

gr::plugin<GR_PLUGIN_CURRENT_ABI_VERSION>& foreignSchedulerPlugin() {
    static gr::plugin<GR_PLUGIN_CURRENT_ABI_VERSION> instance;
    return instance;
}

} // namespace

extern "C" {
void GNURADIO_EXPORT gr_plugin_make(gr_plugin_base** plugin) { *plugin = &foreignSchedulerPlugin(); }

void GNURADIO_EXPORT gr_plugin_free(gr_plugin_base*) {}
}
