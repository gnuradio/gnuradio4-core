#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/DeviceListing.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>

#include "build_configure.hpp"

/**
 * A block library and a plugin register device listings of invented units into the process's registry. The loader
 * lists the units of every family they register. A file's static initializers run once in a process. The cases share
 * one loader over the directory, and the first case that asks for it loads it.
 */
namespace qa_device_listing {

using namespace gr;

[[nodiscard]] std::string listingDirectory() { return std::string(TESTS_BINARY_PATH) + "/device_listing"; }

[[nodiscard]] std::string duplicateDirectory() { return std::string(TESTS_BINARY_PATH) + "/device_listing_duplicate"; }

[[nodiscard]] std::string staleDirectory() { return std::string(TESTS_BINARY_PATH) + "/device_listing_stale"; }

[[nodiscard]] PluginLoader& listingLoader() {
    static PluginLoader loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), std::vector<std::string>{listingDirectory()});
    return loader;
}

[[nodiscard]] const ListedFamily* familyNamed(const DeviceList& list, std::string_view family) {
    const auto found = std::ranges::find(list.families, family, &ListedFamily::family);
    return found == list.families.end() ? nullptr : std::addressof(*found);
}

[[nodiscard]] std::vector<ListedUnit> unitsOf(const DeviceList& list, std::string_view family) {
    std::vector<ListedUnit> units;
    std::ranges::copy_if(list.units, std::back_inserter(units), [family](const ListedUnit& unit) { return unit.family == family; });
    return units;
}

[[nodiscard]] bool holds(std::string_view family) { return std::ranges::contains(gr::globalDeviceListingRegistry().listings(), family, &DeviceListing::family); }

std::vector<DeviceUnit> firstUnits() { return {{"first", "fake:1"}}; }

std::vector<DeviceUnit> secondUnits() { return {{"second", "fake:2"}}; }

} // namespace qa_device_listing

const boost::ut::suite<"DeviceListing"> deviceListingTests = [] {
    using namespace boost::ut;
    using namespace qa_device_listing;

    "the first registration of a family word holds it, and add refuses a second one"_test = [] {
        DeviceListingRegistry registry;
        expect(registry.add({.family = "fake", .opens = false, .receive = true, .transmit = false, .listUnits = firstUnits}));
        expect(!registry.add({.family = "fake", .opens = false, .receive = true, .transmit = false, .listUnits = firstUnits})) << "the same function again";
        expect(!registry.add({.family = "fake", .opens = true, .receive = false, .transmit = true, .listUnits = secondUnits})) << "another function under the word";
        expect(!registry.add({.family = "other", .opens = false, .receive = true, .transmit = false, .listUnits = nullptr})) << "a listing without a function";
        expect(!registry.add({.family = "", .opens = false, .receive = true, .transmit = false, .listUnits = secondUnits})) << "a listing without a family word";

        expect(fatal(eq(registry.listings().size(), 1UZ)));
        const DeviceListing& held = registry.listings().front();
        expect(held.listUnits == &firstUnits) << "the first registration keeps the word";
        expect(!held.opens && held.receive && !held.transmit) << "and its flags";
    };

    "the loader lists the families of a block library and a plugin, and leaves out the one whose listing opens"_test = [] {
        const PluginLoader& loader = listingLoader();
        expect(that % loader.failedPlugins().empty()) << "neither file fails to load";
        expect(eq(loader.plugins().size(), 1UZ)) << "the plugin loads as a plugin";
        expect(fatal(eq(loader.blockLibraries().size(), 1UZ))) << "a library that registers listings and no block is kept mapped";
        expect(eq(loader.blockLibraries().front().nBlockRegistrations, 0UZ));
        expect(eq(loader.blockLibraries().front().nSchedulerRegistrations, 0UZ));

        const DeviceList list = loader.listDevices();
        expect(std::ranges::is_sorted(list.families, {}, &ListedFamily::family)) << "the families are ordered by word";
        expect(eq(list.families.size(), gr::globalDeviceListingRegistry().listings().size())) << "every family the process holds is reported";
        for (const std::string_view word : {"fakeempty", "fakelibrary", "fakeopening", "fakeplugin", "fakethrowing"}) {
            expect(familyNamed(list, word) != nullptr) << word << " is reported";
        }

        const ListedFamily* opening = familyNamed(list, "fakeopening");
        expect(fatal(opening != nullptr));
        expect(opening->opens);
        expect(!opening->listed) << "the default call does not ask a family whose listing opens a unit";
        expect(eq(opening->reason, std::string("its listing opens a unit, and the call does not allow that")));
        expect(that % unitsOf(list, "fakeopening").empty());

        const ListedFamily* library = familyNamed(list, "fakelibrary");
        expect(fatal(library != nullptr));
        expect(library->listed && !library->opens);
        expect(that % library->reason.empty()) << "a family that lists units carries no reason";
        const std::vector<ListedUnit> libraryUnits = unitsOf(list, "fakelibrary");
        expect(fatal(eq(libraryUnits.size(), 3UZ)));
        expect(eq(libraryUnits[0].label, std::string("Fake unit A")));
        expect(eq(libraryUnits[0].selector, std::string("fakelibrary:serial=FAKE0001"))) << "the selector as the family spelled it";
        expect(libraryUnits[0].receive && libraryUnits[0].transmit) << "the directions of the family";
        expect(eq(libraryUnits[1].selector, std::string("fakelibrary:serial=FAKE0002")));
        expect(eq(libraryUnits[2].label, std::string("Fake unit without a serial")));
        expect(that % libraryUnits[2].selector.empty()) << "a unit listed without a selector keeps its label";

        const std::vector<ListedUnit> pluginUnits = unitsOf(list, "fakeplugin");
        expect(fatal(eq(pluginUnits.size(), 1UZ))) << "a plugin's file registers into the same registry";
        expect(eq(pluginUnits[0].selector, std::string("fakeplugin:index=0")));
        expect(pluginUnits[0].receive && !pluginUnits[0].transmit);

        const ListedFamily* throwing = familyNamed(list, "fakethrowing");
        expect(fatal(throwing != nullptr));
        expect(!throwing->listed) << "a family whose listing throws is not listed";
        expect(eq(throwing->reason, std::string("the fake bus does not answer"))) << "and carries the exception's message";

        const ListedFamily* empty = familyNamed(list, "fakeempty");
        expect(fatal(empty != nullptr));
        expect(empty->listed);
        expect(eq(empty->reason, std::string("the listing found no unit")));
        expect(that % unitsOf(list, "fakeempty").empty());
    };

    "with opening allowed the family whose listing opens a unit is asked"_test = [] {
        const DeviceList list = listingLoader().listDevices(Opening::Allowed);

        const ListedFamily* opening = familyNamed(list, "fakeopening");
        expect(fatal(opening != nullptr));
        expect(opening->opens && opening->listed);
        expect(that % opening->reason.empty());
        const std::vector<ListedUnit> units = unitsOf(list, "fakeopening");
        expect(fatal(eq(units.size(), 1UZ)));
        expect(eq(units[0].selector, std::string("fakeopening:bus=1")));
        expect(units[0].receive && !units[0].transmit);
        expect(eq(unitsOf(list, "fakelibrary").size(), unitsOf(listingLoader().listDevices(), "fakelibrary").size())) << "a family that opens nothing lists the same units either way";
    };

    "a library whose only listing names a held family word stays mapped, and the holder answers for the word"_test = [] {
        std::ignore                                = listingLoader();
        const std::size_t              nHeldBefore = gr::globalDeviceListingRegistry().listings().size();
        const std::vector<std::string> directories{duplicateDirectory()};
        const PluginLoader             loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);

        expect(that % loader.failedPlugins().empty()) << "a refused listing fails no file";
        expect(eq(loader.blockLibraries().size(), 1UZ)) << "the library offered a listing, and is kept mapped";
        expect(eq(gr::globalDeviceListingRegistry().listings().size(), nHeldBefore)) << "the registry holds no second listing";

        const DeviceList              list  = loader.listDevices();
        const std::vector<ListedUnit> units = unitsOf(list, "fakelibrary");
        expect(fatal(eq(units.size(), 3UZ))) << "the holder answers for the word";
        expect(eq(units[0].selector, std::string("fakelibrary:serial=FAKE0001")));
        expect(eq(std::ranges::count(list.families, std::string("fakelibrary"), &ListedFamily::family), 1L)) << "the word is reported once";
    };

    "a file refused for its own or a scheduler's plugin ABI version leaves no device listing behind"_test = [] {
        const std::size_t              nHeldBefore = gr::globalDeviceListingRegistry().listings().size();
        const std::vector<std::string> directories{staleDirectory()};
        const PluginLoader             loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);

        struct Refusal {
            std::string_view file;
            std::string_view reasonStart;
            std::string_view family;
        };
        constexpr std::array<Refusal, 3UZ> kRefusals{{
            {"device_listing_stale_library", "scheduler test::stale_listing_scheduler has plugin ABI version 1", "fakestale"},
            {"device_listing_stale_plugin", "plugin ABI version 1 does not match", "fakestaleplugin"},
            {"device_listing_foreign_scheduler_plugin", "scheduler test::listing_plugin_foreign_scheduler has plugin ABI version 1", "fakeforeignscheduler"},
        }};
        expect(eq(loader.failedPlugins().size(), kRefusals.size())) << "every file is refused";
        expect(that % loader.blockLibraries().empty()) << "no library is kept";
        expect(that % loader.plugins().empty()) << "no plugin is kept";

        const DeviceList list = loader.listDevices(Opening::Allowed);
        for (const Refusal& refusal : kRefusals) {
            const auto failed = std::ranges::find_if(loader.failedPlugins(), [&refusal](const auto& entry) { return entry.first.contains(refusal.file); });
            expect(fatal(failed != loader.failedPlugins().end())) << refusal.file << " is refused";
            expect(failed->second.starts_with(refusal.reasonStart)) << failed->second;
            expect(!holds(refusal.family)) << refusal.family << " left the process's registry with its file";
            expect(familyNamed(list, refusal.family) == nullptr) << "the listing asks no function of " << refusal.file;
        }
        expect(eq(gr::globalDeviceListingRegistry().listings().size(), nHeldBefore)) << "the registry holds as many words as before the load";
    };
};

int main() { /* not needed for UT */ }
