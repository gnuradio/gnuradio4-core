#include <vector>

#include <gnuradio-4.0/DeviceListing.hpp>

/// A shared object, not a plugin, that registers a listing under a family word the device listing library registers too.
namespace {

std::vector<gr::DeviceUnit> duplicateUnits() { return {{"Duplicate unit", "fakelibrary:serial=DUPLICATE"}}; }

const bool registered [[maybe_unused]] = gr::globalDeviceListingRegistry().add({.family = "fakelibrary", .opens = false, .receive = true, .transmit = false, .listUnits = duplicateUnits});

} // namespace
