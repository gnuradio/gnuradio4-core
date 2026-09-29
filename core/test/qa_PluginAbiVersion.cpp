#include <boost/ut.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
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
 * A plugin records the plugin ABI version it was compiled against and a host implements exactly one of them. The
 * two plugins in the directory below differ in nothing else, so one load of that directory shows what each of
 * them gets, and shows it before anything asks either of them for a block. The same holds for the two shared
 * objects that register a scheduler without being plugins: their registrations record the version.
 */
namespace qa_plugin_abi_version {

using namespace gr;

constexpr std::string_view kCurrentKey = "test::abi_probe";
constexpr std::string_view kEarlierKey = "test::abi_probe_v1";

constexpr std::string_view kCurrentSchedulerKey = "test::library_scheduler";
constexpr std::string_view kEarlierSchedulerKey = "test::library_scheduler_v1";

constexpr std::uint8_t kEarlierAbiVersion = 1;

constexpr gr::Size_t kTerminalCount = 1000U;

[[nodiscard]] std::string abiPluginDirectory() { return std::string(TESTS_BINARY_PATH) + "/plugin_abi"; }

[[nodiscard]] std::string schedulerLibraryDirectory() { return std::string(TESTS_BINARY_PATH) + "/scheduler_library"; }

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

    "a plugin of an earlier ABI version is refused and contributes nothing, one of this version loads"_test = [] {
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

        expect(fatal(eq(loader.plugins().size(), 1UZ))) << "only the plugin of this ABI version is held";
        expect(that % loader.isBlockAvailable(kCurrentKey)) << "a plugin of this ABI version offers its blocks";

        std::shared_ptr<BlockModel> block = loader.instantiate(kCurrentKey);
        expect(fatal(block != nullptr)) << "the accepted plugin's factory produced nothing";
        expect(eq(std::string(block->typeName()), std::string("gr::testing::AbiProbe")));
    };

    "a shared object whose scheduler records an earlier ABI version is refused and closed, one of this version runs"_test = [] {
        const std::vector<std::string> directories{schedulerLibraryDirectory()};
        PluginLoader                   loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);

        const auto refused = std::ranges::find_if(loader.failedPlugins(), [](const auto& entry) { return entry.first.contains("scheduler_library_v1"); });
        expect(fatal(refused != loader.failedPlugins().end())) << "the library of the earlier ABI version has to be reported as a failure";
        expect(eq(refused->second, std::format("scheduler {} has plugin ABI version {}, which does not match the host's plugin ABI version {}", kEarlierSchedulerKey, kEarlierAbiVersion, GR_PLUGIN_CURRENT_ABI_VERSION))) << "the reason names the scheduler and both versions";

        expect(that % !loader.isSchedulerAvailable(kEarlierSchedulerKey)) << "a refused library offers no scheduler";
        expect(that % !std::ranges::contains(loader.availableSchedulers(), std::string(kEarlierSchedulerKey)));
        expect(loader.instantiateScheduler(kEarlierSchedulerKey) == nullptr) << "a refused library's scheduler cannot be created";
        expect(that % !gr::globalSchedulerRegistry().contains(kEarlierSchedulerKey));

        expect(fatal(eq(loader.blockLibraries().size(), 1UZ))) << "only the library of this ABI version is kept";
        const PluginLoader::BlockLibrary& kept = loader.blockLibraries().front();
        expect(kept.file.ends_with("/libscheduler_library.so")) << kept.file;
        expect(eq(kept.nSchedulerRegistrations, 1UZ));
        expect(that % isMapped(kept.file)) << "the probe sees a library the loader keeps";
        expect(that % !isMapped(refused->first)) << "a refused library is closed";

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
};

int main() { /* not needed for UT */ }
