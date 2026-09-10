#include <boost/ut.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Plugin.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>

#include "build_configure.hpp"

/**
 * A plugin records the plugin ABI version it was compiled against and a host implements exactly one of them. The
 * two plugins in the directory below differ in nothing else, so one load of that directory shows what each of
 * them gets, and shows it before anything asks either of them for a block.
 */
namespace qa_plugin_abi_version {

using namespace gr;

constexpr std::string_view kCurrentKey = "test::abi_probe";
constexpr std::string_view kEarlierKey = "test::abi_probe_v1";

constexpr std::uint8_t kEarlierAbiVersion = 1;

[[nodiscard]] std::string abiPluginDirectory() { return std::string(TESTS_BINARY_PATH) + "/plugin_abi"; }

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
};

int main() { /* not needed for UT */ }
