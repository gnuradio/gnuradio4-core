#ifndef GNURADIO_DEVICE_LISTING_HPP
#define GNURADIO_DEVICE_LISTING_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Export.hpp>

namespace gr {

class PluginLoader;

/// One attached unit as its family's listing reports it.
struct DeviceUnit {
    std::string label;    ///< a name for a person, empty where the family gives none
    std::string selector; ///< the string that names this unit to the family's blocks, empty where the family has none
};

/**
 * @brief The listing a device family registers beside its blocks.
 *
 * `listUnits` runs on the thread that calls `PluginLoader::listDevices()`, one family after another, and two threads
 * may call it at once. Core compares and orders family words and passes the selector on unread; it gives neither a
 * meaning. `opens` states whether `listUnits` opens a unit to find it. `receive` and `transmit` state the directions of
 * the family's blocks, not those of a unit's hardware. `listUnits` may throw. The family is then reported as not
 * listed, with the exception's message.
 */
struct DeviceListing {
    using ListFunction = std::vector<DeviceUnit> (*)();

    std::string  family;
    bool         opens     = false;
    bool         receive   = false;
    bool         transmit  = false;
    ListFunction listUnits = nullptr;
};

/**
 * @brief The device listings of a program and the libraries it loads, at most one per family word.
 *
 * A program and its block libraries register into `globalDeviceListingRegistry()`, usually from a static initializer.
 * The first registration of a family word holds it. A listing states no version, and the family word alone is the key.
 * A file the plugin loader refuses leaves no listing behind. The registry takes no lock. A program does not call
 * `add()` or load a library while another thread calls `listDevices()`.
 */
class DeviceListingRegistry {
    // `add()` is inline. Each library writes `_listings` and `_nOffered` through its own copy of it. The host and every
    // library it loads therefore share the layout of this registry and of `DeviceListing`, which carries no version. A
    // host cannot load a library built against another layout of either.
    std::vector<DeviceListing> _listings;
    std::size_t                _nOffered = 0UZ; // every listing offered to add(), held or not

    friend class PluginLoader;

public:
    /// Holds `listing` under its family word. Returns false, and holds nothing, when the word is empty, a registration
    /// holds the word or the listing carries no function.
    bool add(DeviceListing listing) {
        ++_nOffered;
        if (listing.family.empty() || listing.listUnits == nullptr || std::ranges::contains(_listings, listing.family, &DeviceListing::family)) {
            return false;
        }
        _listings.push_back(std::move(listing));
        return true;
    }

    /// the listings held, in the order they were registered
    [[nodiscard]] const std::vector<DeviceListing>& listings() const noexcept { return _listings; }
};

/// The registry a program and the libraries it loads register their device listings into.
GNURADIO_EXPORT
DeviceListingRegistry& globalDeviceListingRegistry();

/// Whether a listDevices() call asks a family whose listing opens a unit.
enum class Opening : std::uint8_t {
    Avoided, ///< such a family is left out, and reported with the reason
    Allowed, ///< every family is asked
};

/// One unit a listDevices() call found, with the word and the directions of its family.
struct ListedUnit {
    std::string family;
    std::string label;
    std::string selector;
    bool        receive  = false;
    bool        transmit = false;
};

/// What one listDevices() call did with one family.
struct ListedFamily {
    std::string family;
    bool        opens  = false; ///< the flag the family states
    bool        listed = false; ///< whether the call asked the family and the family answered
    std::string reason;         ///< why the family reported no unit, empty where it reported one
};

/// The units a listDevices() call found, and every family it asked or left out, both ordered by family word.
struct DeviceList {
    std::vector<ListedUnit>   units;
    std::vector<ListedFamily> families;
};

namespace detail {

/// the units of each of `listings` that `opening` lets the call ask, and a report of every one of them
[[nodiscard]] inline DeviceList listDevices(std::vector<DeviceListing> listings, Opening opening) {
    std::ranges::stable_sort(listings, {}, &DeviceListing::family);
    DeviceList list;
    for (const DeviceListing& listing : listings) {
        ListedFamily& family = list.families.emplace_back(ListedFamily{.family = listing.family, .opens = listing.opens, .listed = false, .reason = {}});
        if (listing.opens && opening == Opening::Avoided) {
            family.reason = "its listing opens a unit, and the call does not allow that";
            continue;
        }
        std::vector<DeviceUnit> units;
        try {
            units = listing.listUnits();
        } catch (const std::exception& error) {
            family.reason = error.what();
            continue;
        } catch (...) {
            family.reason = "the listing failed with an exception that carries no message";
            continue;
        }
        family.listed = true;
        if (units.empty()) {
            family.reason = "the listing found no unit";
        }
        for (DeviceUnit& unit : units) {
            list.units.push_back({.family = listing.family, .label = std::move(unit.label), .selector = std::move(unit.selector), .receive = listing.receive, .transmit = listing.transmit});
        }
    }
    return list;
}

} // namespace detail

} // namespace gr

#endif // GNURADIO_DEVICE_LISTING_HPP
