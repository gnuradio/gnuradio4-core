#include <stdexcept>
#include <vector>

#include <gnuradio-4.0/DeviceListing.hpp>

/**
 * @brief A shared object that registers device listings and no block, and is not a plugin.
 *
 * Its families list fixed test units and open nothing. One family lists units, one of them without a selector. One
 * states that its listing opens a unit. One throws. One finds no unit.
 */
namespace {

std::vector<gr::DeviceUnit> fakeLibraryUnits() { return {{"Fake unit A", "fakelibrary:serial=FAKE0001"}, {"Fake unit B", "fakelibrary:serial=FAKE0002"}, {"Fake unit without a serial", ""}}; }

std::vector<gr::DeviceUnit> fakeOpeningUnits() { return {{"Fake opened unit", "fakeopening:bus=1"}}; }

std::vector<gr::DeviceUnit> fakeThrowingUnits() { throw std::runtime_error("the fake bus does not answer"); }

std::vector<gr::DeviceUnit> fakeEmptyUnits() { return {}; }

const bool registered [[maybe_unused]] = [] {
    gr::DeviceListingRegistry& registry = gr::globalDeviceListingRegistry();
    registry.add({.family = "fakelibrary", .opens = false, .receive = true, .transmit = true, .listUnits = fakeLibraryUnits});
    registry.add({.family = "fakeopening", .opens = true, .receive = true, .transmit = false, .listUnits = fakeOpeningUnits});
    registry.add({.family = "fakethrowing", .opens = false, .receive = true, .transmit = false, .listUnits = fakeThrowingUnits});
    registry.add({.family = "fakeempty", .opens = false, .receive = false, .transmit = true, .listUnits = fakeEmptyUnits});
    return true;
}();

} // namespace
