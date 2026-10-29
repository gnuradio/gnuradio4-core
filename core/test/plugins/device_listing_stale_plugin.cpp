#include <vector>

#include <gnuradio-4.0/DeviceListing.hpp>
#include <gnuradio-4.0/Plugin.hpp>

/// A plugin at plugin ABI version 1 that adds a device listing to the process's registry when it loads.
namespace {

std::vector<gr::DeviceUnit> fakeStalePluginUnits() { return {{"Fake stale plugin unit", "fakestaleplugin:index=0"}}; }

const bool registered [[maybe_unused]] = gr::globalDeviceListingRegistry().add({.family = "fakestaleplugin", .opens = false, .receive = true, .transmit = false, .listUnits = fakeStalePluginUnits});

gr::plugin<1>& stalePlugin() {
    static gr::plugin<1> instance;
    return instance;
}

} // namespace

extern "C" {
void GNURADIO_EXPORT gr_plugin_make(gr_plugin_base** plugin) { *plugin = &stalePlugin(); }

void GNURADIO_EXPORT gr_plugin_free(gr_plugin_base*) {}
}
