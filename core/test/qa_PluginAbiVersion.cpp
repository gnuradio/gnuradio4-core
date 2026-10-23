#include <boost/ut.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Plugin.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>

#include "MappedLibrary.hpp"
#include "build_configure.hpp"

/**
 * A plugin records the plugin ABI version it was compiled against and a host implements exactly one of them. Two of
 * the plugins in the directory below differ in nothing else, so one load of that directory shows what each of them
 * gets, and shows it before anything asks either of them for a block. The third is at the host's version and
 * registers a scheduler at another one when it loads. A fourth plugin records the version below the host's and has a
 * directory of its own. The three shared objects that register a scheduler without being plugins differ in the same
 * way as the first two. Their registrations record a version, or none.
 */
// ABI 10's SchedulerModel has no stopAfterDrain slot. Do not let a version
// rollback silently admit that vtable into a host which invokes drain.
static_assert(GR_PLUGIN_CURRENT_ABI_VERSION >= 11);

namespace qa_plugin_abi_version {

using namespace gr;

constexpr std::string_view kCurrentKey  = "test::abi_probe";
constexpr std::string_view kEarlierKey  = "test::abi_probe_v1";
constexpr std::string_view kPreviousKey = "test::abi_probe_previous";

constexpr std::string_view kCurrentSchedulerKey     = "test::library_scheduler";
constexpr std::string_view kEarlierSchedulerKey     = "test::library_scheduler_v1";
constexpr std::string_view kUnversionedSchedulerKey = "test::library_scheduler_unversioned";
constexpr std::string_view kForeignSchedulerKey     = "test::plugin_foreign_scheduler";

constexpr std::uint8_t kEarlierAbiVersion = 1;

// the version below the host's, which the probe in a directory of its own records
constexpr std::uint8_t kPreviousAbiVersion = GR_PLUGIN_CURRENT_ABI_VERSION - 1;

constexpr gr::Size_t kTerminalCount = 1000U;

[[nodiscard]] std::string abiPluginDirectory() { return std::string(TESTS_BINARY_PATH) + "/plugin_abi"; }

[[nodiscard]] std::string previousPluginDirectory() { return std::string(TESTS_BINARY_PATH) + "/plugin_abi_previous"; }

[[nodiscard]] std::string schedulerLibraryDirectory() { return std::string(TESTS_BINARY_PATH) + "/scheduler_library"; }

// the reason the loader gives for a plugin of an earlier version
[[nodiscard]] std::string earlierPluginReason() { return std::format("plugin ABI version {} does not match the host's plugin ABI version {}", kEarlierAbiVersion, GR_PLUGIN_CURRENT_ABI_VERSION); }

// the reason the loader gives for a file whose load registered a scheduler at an earlier version
[[nodiscard]] std::string earlierSchedulerReason(std::string_view key) { return std::format("scheduler {} has plugin ABI version {}, which does not match the host's plugin ABI version {}", key, kEarlierAbiVersion, GR_PLUGIN_CURRENT_ABI_VERSION); }

// the reason the loader gives for a file whose load registered a scheduler without a version
[[nodiscard]] std::string unversionedSchedulerReason(std::string_view key) { return std::format("scheduler {} carries no plugin ABI version; the host's plugin ABI version is {}", key, GR_PLUGIN_CURRENT_ABI_VERSION); }

// two factories with distinct bodies, which identical-code folding cannot merge into one address
[[nodiscard]] std::unique_ptr<SchedulerModel> makeNoScheduler(property_map /*parameters*/) { return nullptr; }

[[nodiscard]] std::unique_ptr<SchedulerModel> makeThrowingScheduler(property_map /*parameters*/) { throw std::logic_error("the factory of a replaced entry is never called"); }

using gr::testing::isMapped;

// ends the stream after `n_samples_max` items
struct CountedSource : gr::Block<CountedSource> {
    gr::PortOut<float> out;

    gr::Size_t n_samples_max = 0U;

    GR_MAKE_REFLECTABLE(CountedSource, out, n_samples_max);

    gr::Size_t _nProduced = 0U;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const auto nToPublish = std::min(static_cast<gr::Size_t>(outSpan.size()), n_samples_max - _nProduced);
        std::ranges::fill_n(outSpan.begin(), static_cast<std::ptrdiff_t>(nToPublish), 1.0f);
        outSpan.publish(nToPublish);
        _nProduced += nToPublish;
        return _nProduced == n_samples_max ? gr::work::Status::DONE : gr::work::Status::OK;
    }
};

struct CountingSink : gr::Block<CountingSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(CountingSink, in);

    gr::Size_t _nReceived = 0U;

    void processOne(float) { ++_nReceived; }
};

} // namespace qa_plugin_abi_version

const boost::ut::suite<"PluginAbiVersion"> pluginAbiVersionTests = [] {
    using namespace boost::ut;
    using namespace qa_plugin_abi_version;

    "a plugin of an earlier ABI version or registering a scheduler of one is refused, one of this version loads"_test = [] {
        BlockRegistry                  registry;
        SchedulerRegistry              schedulerRegistry;
        const std::vector<std::string> directories{abiPluginDirectory()};
        PluginLoader                   loader(registry, schedulerRegistry, directories);

        const auto refused = std::ranges::find_if(loader.failedPlugins(), [](const auto& entry) { return entry.first.contains("abi_probe_plugin_v1"); });
        expect(fatal(refused != loader.failedPlugins().end())) << "the plugin of the earlier ABI version has to be reported as a failure";
        expect(eq(refused->second, earlierPluginReason())) << "the reason names both versions";

        expect(that % !loader.isBlockAvailable(kEarlierKey)) << "a refused plugin offers no block";
        expect(loader.instantiate(kEarlierKey) == nullptr) << "a refused plugin's block cannot be created";
        expect(that % !registry.contains(kEarlierKey));
        expect(that % !gr::globalBlockRegistry().contains(kEarlierKey));
        expect(that % loader.blockLibraries().empty()) << "a refused plugin is not taken up again as a shared object of blocks";

        const auto foreign = std::ranges::find_if(loader.failedPlugins(), [](const auto& entry) { return entry.first.contains("abi_probe_plugin_foreign_scheduler"); });
        expect(fatal(foreign != loader.failedPlugins().end())) << "a plugin whose load registered a scheduler of an earlier ABI version has to be reported as a failure";
        expect(eq(foreign->second, earlierSchedulerReason(kForeignSchedulerKey))) << "the reason names the scheduler and both versions";
        expect(that % !loader.isSchedulerAvailable(kForeignSchedulerKey));
        expect(that % !gr::globalSchedulerRegistry().contains(kForeignSchedulerKey)) << "the scheduler it registered is dropped with it";

        expect(fatal(eq(loader.plugins().size(), 1UZ))) << "only the plugin of this ABI version is held";
        expect(that % loader.isBlockAvailable(kCurrentKey)) << "a plugin of this ABI version offers its blocks";

        std::shared_ptr<BlockModel> block = loader.instantiate(kCurrentKey);
        expect(fatal(block != nullptr)) << "the accepted plugin's factory produced nothing";
        expect(eq(std::string(block->typeName()), std::string("gr::testing::AbiProbe")));
    };

    "a plugin that records the previous ABI version is refused"_test = [] {
        BlockRegistry                  registry;
        SchedulerRegistry              schedulerRegistry;
        const std::vector<std::string> directories{previousPluginDirectory()};
        PluginLoader                   loader(registry, schedulerRegistry, directories);

        const auto refused = std::ranges::find_if(loader.failedPlugins(), [](const auto& entry) { return entry.first.contains("abi_probe_plugin_previous"); });
        expect(fatal(refused != loader.failedPlugins().end())) << "the plugin of the previous version has to be reported as a failure";
        expect(eq(refused->second, std::format("plugin ABI version {} does not match the host's plugin ABI version {}", kPreviousAbiVersion, GR_PLUGIN_CURRENT_ABI_VERSION))) << "the reason names both versions";

        const auto schedulerRefused = std::ranges::find_if(loader.failedPlugins(), [](const auto& entry) { return entry.first.contains("abi_probe_scheduler_previous"); });
        expect(fatal(schedulerRefused != loader.failedPlugins().end()));
        expect(eq(schedulerRefused->second, std::format("plugin ABI version {} does not match the host's plugin ABI version {}", kPreviousAbiVersion, GR_PLUGIN_CURRENT_ABI_VERSION)));
        expect(!loader.isSchedulerAvailable("test::previous_plugin_scheduler"));
        expect(loader.instantiateScheduler("test::previous_plugin_scheduler") == nullptr) << "the rejected scheduler factory must not be called";

        expect(that % loader.plugins().empty()) << "the loader holds no plugin of the previous version";
        expect(that % !loader.isBlockAvailable(kPreviousKey)) << "a refused plugin offers no block";
        expect(loader.instantiate(kPreviousKey) == nullptr) << "a refused plugin's block cannot be created";
        expect(that % !registry.contains(kPreviousKey));
        expect(that % !gr::globalBlockRegistry().contains(kPreviousKey));
        expect(that % loader.blockLibraries().empty()) << "a refused plugin is not taken up again as a shared object of blocks";
    };

    "a shared object whose scheduler records an earlier ABI version or none is refused and stays mapped, one of this version runs"_test = [] {
        const std::vector<std::string> directories{schedulerLibraryDirectory()};
        PluginLoader                   loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);

        const auto refused = std::ranges::find_if(loader.failedPlugins(), [](const auto& entry) { return entry.first.contains("scheduler_library_v1"); });
        expect(fatal(refused != loader.failedPlugins().end())) << "the library of the earlier ABI version has to be reported as a failure";
        expect(eq(refused->second, earlierSchedulerReason(kEarlierSchedulerKey))) << "the reason names the scheduler and both versions";

        const auto unversioned = std::ranges::find_if(loader.failedPlugins(), [](const auto& entry) { return entry.first.contains("scheduler_library_unversioned"); });
        expect(fatal(unversioned != loader.failedPlugins().end())) << "the library whose scheduler records no version has to be reported as a failure";
        expect(eq(unversioned->second, unversionedSchedulerReason(kUnversionedSchedulerKey))) << "the reason names the scheduler and the host's version";
        expect(that % !loader.isSchedulerAvailable(kUnversionedSchedulerKey));
        expect(that % !gr::globalSchedulerRegistry().contains(kUnversionedSchedulerKey)) << "its entry is dropped with it";

        expect(that % !loader.isSchedulerAvailable(kEarlierSchedulerKey)) << "a refused library offers no scheduler";
        expect(that % !std::ranges::contains(loader.availableSchedulers(), std::string(kEarlierSchedulerKey)));
        expect(loader.instantiateScheduler(kEarlierSchedulerKey) == nullptr) << "a refused library's scheduler cannot be created";
        expect(that % !gr::globalSchedulerRegistry().contains(kEarlierSchedulerKey));

        expect(fatal(eq(loader.blockLibraries().size(), 1UZ))) << "only the library of this ABI version is kept";
        const PluginLoader::BlockLibrary& kept = loader.blockLibraries().front();
        expect(kept.file.ends_with("/libscheduler_library.so")) << kept.file;
        expect(eq(kept.nSchedulerRegistrations, 1UZ));
        expect(that % isMapped(kept.file)) << "the probe sees a library the loader keeps";
        expect(that % !std::ranges::contains(loader.blockLibraries(), refused->first, &PluginLoader::BlockLibrary::file)) << "a refused library is not kept";
        expect(that % !std::ranges::contains(loader.blockLibraries(), unversioned->first, &PluginLoader::BlockLibrary::file)) << "a refused library is not kept";
        expect(that % loader.plugins().empty()) << "the loader holds no handle to a library that is not a plugin, the refused one included";

        expect(that % isMapped(refused->first)) << "the loader unmapped a library it refused for the version of its scheduler";
        expect(that % isMapped(unversioned->first)) << "the loader unmapped a library it refused for a scheduler without a version";

        expect(that % loader.isSchedulerAvailable(kCurrentSchedulerKey)) << "a library of this ABI version offers its scheduler";
        expect(that % std::ranges::contains(loader.availableSchedulers(), std::string(kCurrentSchedulerKey)));
        expect(gr::globalSchedulerRegistry().abiVersion(kCurrentSchedulerKey) == std::optional<std::uint8_t>{GR_PLUGIN_CURRENT_ABI_VERSION}) << "the registration recorded the version its library was built against";

        std::shared_ptr<SchedulerModel> scheduler = loader.instantiateScheduler(kCurrentSchedulerKey);
        expect(fatal(scheduler != nullptr)) << "the kept library's factory produced nothing";

        gr::Graph      flow;
        CountedSource& source = flow.emplaceBlock<CountedSource>({{"n_samples_max", kTerminalCount}});
        CountingSink&  sink   = flow.emplaceBlock<CountingSink>();
        expect(fatal(flow.connect<"out", "in">(source, sink).has_value()));
        scheduler->setGraph(std::move(flow));

        const std::expected<void, Error> result = scheduler->runAndWait();
        expect(result.has_value()) << (result.has_value() ? std::string() : result.error().message);
        expect(scheduler->stopAfterDrain(std::chrono::milliseconds(50))) << "a scheduler rebuilt at the host ABI supports the drain entry";
        expect(eq(source._nProduced, kTerminalCount));
        expect(eq(sink._nReceived, kTerminalCount)) << "the run reached its terminal count";
    };

    // A dlopen of a file that an earlier load mapped returns the same mapping, and the file's static initializers do not
    // run again. Every loader after the first one in the process refuses such a file from the record of its refusal.
    "each loader in the process refuses a file that an earlier loader refused, for the same reason"_test = [] {
        const std::vector<std::pair<std::string, std::string>> refusals{
            {"/libabi_probe_plugin_v1.so", earlierPluginReason()},
            {"/libabi_probe_plugin_foreign_scheduler.so", earlierSchedulerReason(kForeignSchedulerKey)},
            {"/libscheduler_library_v1.so", earlierSchedulerReason(kEarlierSchedulerKey)},
            {"/libscheduler_library_unversioned.so", unversionedSchedulerReason(kUnversionedSchedulerKey)},
        };
        const std::vector<std::string> directories{abiPluginDirectory(), schedulerLibraryDirectory()};

        for (std::size_t loaderIndex = 0UZ; loaderIndex < 3UZ; ++loaderIndex) {
            BlockRegistry     registry;
            SchedulerRegistry schedulerRegistry;
            PluginLoader      loader(registry, schedulerRegistry, directories);

            for (const auto& [fileName, reason] : refusals) {
                const auto refused = std::ranges::find_if(loader.failedPlugins(), [&fileName](const auto& entry) { return entry.first.ends_with(fileName); });
                expect(refused != loader.failedPlugins().end()) << std::format("loader {} admitted {}: plugins={} failed={}", loaderIndex, fileName, loader.plugins().size(), loader.failedPlugins().size());
                if (refused != loader.failedPlugins().end()) {
                    expect(eq(refused->second, reason)) << std::format("the reason of loader {} for {}", loaderIndex, fileName);
                }
            }
            expect(eq(loader.plugins().size(), 1UZ)) << std::format("loader {} holds only the plugin of this ABI version", loaderIndex);
            expect(that % !loader.isSchedulerAvailable(kForeignSchedulerKey)) << std::format("loader {} offers the scheduler of a refused plugin", loaderIndex);
            expect(that % !gr::globalSchedulerRegistry().contains(kForeignSchedulerKey));
        }
    };

    "an entry whose factory a registration without a version replaced carries no version"_test = [] {
        constexpr std::string_view kKey = "test::replaced_scheduler";
        SchedulerRegistry          registry;
        registry.insert(kKey, "", makeNoScheduler);
        expect(registry.abiVersion(kKey) == std::optional<std::uint8_t>{GR_PLUGIN_CURRENT_ABI_VERSION}) << "the first registration recorded this version";

        // replaces the factory and keeps the version, as a registry without versions does on a second registration
        // of the key
        SchedulerRegistry::Entries entries                    = registry.takeEntries();
        entries.handlers.at(std::string(kKey)).createFunction = makeThrowingScheduler;
        registry.restoreEntries(std::move(entries), false);

        expect(that % registry.contains(kKey));
        expect(registry.abiVersion(kKey) == std::nullopt) << "the recorded version belongs to the replaced factory";
        expect(registry.abiVersion("test::no_such_scheduler") == std::nullopt);
    };
};

int main() { /* not needed for UT */ }
