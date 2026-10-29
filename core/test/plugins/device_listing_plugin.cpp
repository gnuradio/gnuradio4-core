#include <vector>

#include <gnuradio-4.0/DeviceListing.hpp>
#include <gnuradio-4.0/Plugin.hpp>

GR_PLUGIN("Device Listing Plugin", "Unknown", "MIT", "v1")

/// A plugin that registers no block and adds a device listing to the process's registry when it loads.
namespace {

std::vector<gr::DeviceUnit> fakePluginUnits() { return {{"Fake plugin unit", "fakeplugin:index=0"}}; }

const bool registered [[maybe_unused]] = gr::globalDeviceListingRegistry().add({.family = "fakeplugin", .opens = false, .receive = true, .transmit = false, .listUnits = fakePluginUnits});

} // namespace
