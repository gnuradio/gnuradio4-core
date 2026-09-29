#include <boost/ut.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <dlfcn.h>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Plugin.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>

#include "build_configure.hpp"

/**
 * A plugin records the plugin ABI version it was compiled against and a host implements exactly one of them. Two of
 * the plugins in the directory below differ in nothing else, so one load of that directory shows what each of them
 * gets, and shows it before anything asks either of them for a block. The third is at the host's version and
 * registers a scheduler at another one when it loads. The same holds for the three shared objects that register a
 * scheduler without being plugins: their registrations record the version, or none.
 */
namespace qa_plugin_abi_version {

using namespace gr;

constexpr std::string_view kCurrentKey = "test::abi_probe";
constexpr std::string_view kEarlierKey = "test::abi_probe_v1";

constexpr std::string_view kCurrentSchedulerKey     = "test::library_scheduler";
constexpr std::string_view kEarlierSchedulerKey     = "test::library_scheduler_v1";
constexpr std::string_view kUnversionedSchedulerKey = "test::library_scheduler_unversioned";
constexpr std::string_view kForeignSchedulerKey     = "test::plugin_foreign_scheduler";

constexpr std::uint8_t kEarlierAbiVersion = 1;

constexpr gr::Size_t kTerminalCount = 1000U;

[[nodiscard]] std::string abiPluginDirectory() { return std::string(TESTS_BINARY_PATH) + "/plugin_abi"; }

[[nodiscard]] std::string schedulerLibraryDirectory() { return std::string(TESTS_BINARY_PATH) + "/scheduler_library"; }

// the reason the loader gives for a file whose load registered a scheduler at an earlier version
[[nodiscard]] std::string earlierSchedulerReason(std::string_view key) { return std::format("scheduler {} has plugin ABI version {}, which does not match the host's plugin ABI version {}", key, kEarlierAbiVersion, GR_PLUGIN_CURRENT_ABI_VERSION); }

// two factories of distinct bodies, so that no folding of identical functions gives them one address
[[nodiscard]] std::unique_ptr<SchedulerModel> makeNoScheduler(property_map /*parameters*/) { return nullptr; }

[[nodiscard]] std::unique_ptr<SchedulerModel> makeThrowingScheduler(property_map /*parameters*/) { throw std::logic_error("the factory of a replaced entry is never called"); }

// whether the process holds the shared object at `file` mapped; the probe takes a reference only when it does
[[nodiscard]] bool isMapped(const std::string& file) {
    void* handle = dlopen(file.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    if (handle == nullptr) {
        return false;
    }
    dlclose(handle);
    return true;
}

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
        expect(eq(refused->second, std::format("plugin ABI version {} does not match the host's plugin ABI version {}", kEarlierAbiVersion, GR_PLUGIN_CURRENT_ABI_VERSION))) << "the reason names both versions";

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

    "a shared object whose scheduler records an earlier ABI version or none is refused and closed, one of this version runs"_test = [] {
        const std::vector<std::string> directories{schedulerLibraryDirectory()};
        PluginLoader                   loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);

        const auto refused = std::ranges::find_if(loader.failedPlugins(), [](const auto& entry) { return entry.first.contains("scheduler_library_v1"); });
        expect(fatal(refused != loader.failedPlugins().end())) << "the library of the earlier ABI version has to be reported as a failure";
        expect(eq(refused->second, earlierSchedulerReason(kEarlierSchedulerKey))) << "the reason names the scheduler and both versions";

        const auto unversioned = std::ranges::find_if(loader.failedPlugins(), [](const auto& entry) { return entry.first.contains("scheduler_library_unversioned"); });
        expect(fatal(unversioned != loader.failedPlugins().end())) << "the library whose scheduler records no version has to be reported as a failure";
        expect(eq(unversioned->second, std::format("scheduler {} carries no plugin ABI version; the host's plugin ABI version is {}", kUnversionedSchedulerKey, GR_PLUGIN_CURRENT_ABI_VERSION))) << "the reason names the scheduler and the host's version";
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

        // glibc marks a shared object NODELETE when a load binds one of its STB_GNU_UNIQUE symbols, and dlclose then leaves it mapped
        std::println(stderr, "the refused library {} is {} after the loader closed its handle; a runtime may keep a closed library mapped, as glibc does when a load bound one of its STB_GNU_UNIQUE symbols", refused->first, isMapped(refused->first) ? "still mapped" : "unmapped");

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
        expect(eq(source._nProduced, kTerminalCount));
        expect(eq(sink._nReceived, kTerminalCount)) << "the run reached its terminal count";
    };

    "an entry whose factory a registration without a version replaced carries no version"_test = [] {
        constexpr std::string_view kKey = "test::replaced_scheduler";
        SchedulerRegistry          registry;
        registry.insert(kKey, "", makeNoScheduler);
        expect(registry.abiVersion(kKey) == std::optional<std::uint8_t>{GR_PLUGIN_CURRENT_ABI_VERSION}) << "the first registration recorded this version";

        // what a registry without versions does on a second registration of the key: the factory changes, the version stays
        SchedulerRegistry::Entries entries                    = registry.takeEntries();
        entries.handlers.at(std::string(kKey)).createFunction = makeThrowingScheduler;
        registry.restoreEntries(std::move(entries), false);

        expect(that % registry.contains(kKey));
        expect(registry.abiVersion(kKey) == std::nullopt) << "the recorded version belongs to the replaced factory";
        expect(registry.abiVersion("test::no_such_scheduler") == std::nullopt);
    };
};

int main() { /* not needed for UT */ }
