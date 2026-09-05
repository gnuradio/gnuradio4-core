#include <boost/ut.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>

#include "build_configure.hpp"

const boost::ut::suite<"PluginRegistration"> pluginRegistrationTests = [] {
    using namespace boost::ut;

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
};

int main() { /* not needed for UT */ }
