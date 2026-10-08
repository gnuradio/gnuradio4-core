#include <boost/ut.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <dlfcn.h>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>

#include "MappedLibrary.hpp"
#include "build_configure.hpp"

namespace qa_plugin_registration {

[[nodiscard]] std::string pluginDirectory() { return std::string(TESTS_BINARY_PATH) + "/plugins"; }

// one plugin at the ABI version this core implements, one at version 1, and one whose load registers a scheduler at version 1
[[nodiscard]] std::string abiPluginDirectory() { return std::string(TESTS_BINARY_PATH) + "/plugin_abi"; }

// the path of the plugin file in directory whose name contains name, in the form the loader opens it
[[nodiscard]] std::string pluginFile(std::string_view name, const std::string& directory = pluginDirectory()) {
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator{std::filesystem::path(directory)}) {
        if (entry.path().filename().string().contains(name)) {
            return entry.path().string();
        }
    }
    return {};
}

using gr::testing::isMapped;

// Opens GoodRegistry for the rest of the run, ahead of any loader. glibc keeps a shared object mapped until exit once
// it provides the first copy of an STB_GNU_UNIQUE symbol, and the order of a directory's loads decides which object
// does. GoodRegistry and GoodBase define the same such symbols. GoodRegistry then provides them, and GoodBase unmaps
// when it is closed.
[[nodiscard]] bool openUniqueSymbolProvider() {
    static void* const handle = dlopen(pluginFile("GoodRegistry").c_str(), RTLD_LAZY | RTLD_LOCAL);
    return handle != nullptr;
}

// Opens the plugin of this ABI version for the rest of the run, ahead of any loader, for the same reason: every such
// symbol of the two plugins the loader refuses is one this plugin defines too.
[[nodiscard]] bool openAbiUniqueSymbolProvider() {
    static void* const handle = dlopen(pluginFile("abi_probe_plugin.", abiPluginDirectory()).c_str(), RTLD_LAZY | RTLD_LOCAL);
    return handle != nullptr;
}

} // namespace qa_plugin_registration

const boost::ut::suite<"PluginRegistration"> pluginRegistrationTests = [] {
    using namespace boost::ut;
    using namespace qa_plugin_registration;

    // A plugin's code can run after its loader is gone: in a block or a scheduler that the loader created, and in a
    // pool or a provider that the plugin registered when it loaded. This case comes first: the cases after it leave
    // the plugin mapped.
    "a plugin that has created nothing stays mapped after its loader is gone"_test = [] {
        const std::string file = pluginFile("GoodBasePlugin");
        expect(fatal(!file.empty())) << "the plugin file was not found";
        expect(fatal(openUniqueSymbolProvider())) << "the plugin that provides the shared symbols did not open";
        expect(fatal(!isMapped(file))) << "the plugin is mapped before its first load";
        {
            gr::BlockRegistry              registry;
            gr::SchedulerRegistry          schedulerRegistry;
            const std::vector<std::string> pluginDirectories{pluginDirectory()};
            gr::PluginLoader               loader(registry, schedulerRegistry, pluginDirectories);
            expect(fatal(loader.isBlockAvailable("good::fixed_source<float64>"))) << "the plugin registered no block";
            expect(fatal(isMapped(file))) << "the loaded plugin is not mapped";
        }
        expect(isMapped(file)) << "the loader unmapped a plugin that has created nothing";
    };

    "a plugin registers into the instance its own header declares"_test = [] {
        gr::BlockRegistry              registry;
        gr::SchedulerRegistry          schedulerRegistry;
        const std::vector<std::string> pluginDirectories{std::string(TESTS_BINARY_PATH) + "/plugins"};
        gr::PluginLoader               loader(registry, schedulerRegistry, pluginDirectories);

        for (const auto& [file, reason] : loader.failedPlugins()) {
            expect(file.find("Good") == std::string::npos) << std::format("{} failed to load: {}", file, reason);
        }

        const std::vector<std::string> available = loader.availableBlocks();
        for (const std::string_view name : {"good::identity<float32>", "good::identity<float64>", "good::cout_sink<float32>", "good::fixed_source<float64>"}) {
            expect(std::ranges::find(available, name) != available.end()) << std::format("{} is not provided by any loaded plugin", name);
        }
        expect(loader.instantiate("good::identity<float32>") != nullptr);
    };

    // The first case leaves the plugin mapped, and this case passes whenever that one does. It separates only two
    // loaders that fail the first case: one that keeps a plugin once it has created a block, and one that keeps none.
    "a plugin that has created a block stays mapped after its loader is gone"_test = [] {
        const std::string file = pluginFile("GoodBasePlugin");
        expect(fatal(!file.empty())) << "the plugin file was not found";
        expect(fatal(openUniqueSymbolProvider())) << "the plugin that provides the shared symbols did not open";
        {
            gr::BlockRegistry              registry;
            gr::SchedulerRegistry          schedulerRegistry;
            const std::vector<std::string> pluginDirectories{pluginDirectory()};
            gr::PluginLoader               loader(registry, schedulerRegistry, pluginDirectories);
            expect(fatal(loader.instantiate("good::fixed_source<float64>") != nullptr)) << "the plugin created no block";
            expect(fatal(isMapped(file))) << "the loaded plugin is not mapped";
        }
        expect(isMapped(file)) << "the loader unmapped a plugin that has created a block";
    };

    // A plugin's code runs when it loads, before the loader checks its ABI version and the versions of the schedulers its
    // load registered. A pool or a provider registered then can point into it after the loader refuses it.
    "a plugin refused after its code ran stays mapped after its loader is gone"_test = [] {
        const std::string earlier = pluginFile("abi_probe_plugin_v1", abiPluginDirectory());
        const std::string foreign = pluginFile("abi_probe_plugin_foreign_scheduler", abiPluginDirectory());
        expect(fatal(!earlier.empty() && !foreign.empty())) << "the plugin files were not found";
        expect(fatal(openAbiUniqueSymbolProvider())) << "the plugin that provides the shared symbols did not open";
        expect(fatal(!isMapped(earlier) && !isMapped(foreign))) << "a refused plugin is mapped before its first load";
        {
            gr::BlockRegistry              registry;
            gr::SchedulerRegistry          schedulerRegistry;
            const std::vector<std::string> pluginDirectories{abiPluginDirectory()};
            gr::PluginLoader               loader(registry, schedulerRegistry, pluginDirectories);
            expect(fatal(loader.failedPlugins().contains(earlier))) << "the plugin of ABI version 1 was not refused";
            expect(fatal(loader.failedPlugins().contains(foreign))) << "the plugin that registered a scheduler of ABI version 1 was not refused";
            expect(eq(loader.plugins().size(), 1UZ)) << "the loader admitted a refused plugin";
            expect(that % loader.blockLibraries().empty()) << "the loader took a refused plugin as a shared object of blocks";
        }
        expect(isMapped(earlier)) << "the loader unmapped a plugin it refused for its ABI version";
        expect(isMapped(foreign)) << "the loader unmapped a plugin it refused for the version of a scheduler it registered";
    };
};

int main() { /* not needed for UT */ }
