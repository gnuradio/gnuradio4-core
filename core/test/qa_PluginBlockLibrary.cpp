#include <boost/ut.hpp>

#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>

#include "build_configure.hpp"

/**
 * A plugin directory may hold shared objects that are not plugins: they export no `gr_plugin_make` and register
 * their blocks from static initializers as they are mapped. The scan opens every file it finds, so it opens these
 * too, and unloading one afterwards would leave its registry entries pointing into an unmapped page.
 */
namespace qa_plugin_block_library {

using namespace gr;

constexpr std::string_view kDoublerKey   = "test::library_doubler";
constexpr std::string_view kTriplerKey   = "test::library_tripler";
constexpr std::string_view kDuplicateKey = "test::library_duplicate";

[[nodiscard]] std::string blockLibraryDirectory() { return std::string(TESTS_BINARY_PATH) + "/block_library"; }

[[nodiscard]] std::string duplicateLibraryDirectory() { return std::string(TESTS_BINARY_PATH) + "/block_library_duplicate"; }

[[nodiscard]] std::string skippedNamesDirectory() { return std::string(TESTS_BINARY_PATH) + "/block_library_skipped"; }

[[nodiscard]] std::string pluginDirectory() { return std::string(TESTS_BINARY_PATH) + "/plugins"; }

} // namespace qa_plugin_block_library

const boost::ut::suite<"PluginBlockLibrary"> pluginBlockLibraryTests = [] {
    using namespace boost::ut;
    using namespace qa_plugin_block_library;

    "every block library in a directory is kept mapped, and their blocks outlive the loader"_test = [] {
        {
            const std::vector<std::string> directories{blockLibraryDirectory()};
            PluginLoader                   loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);

            expect(fatal(eq(loader.blockLibraries().size(), 2UZ))) << "both shared objects have to be recognized as block libraries";
            for (const PluginLoader::BlockLibrary& library : loader.blockLibraries()) {
                expect(library.file.contains("block_library")) << std::format("unexpected block library {}", library.file);
                expect(ge(library.nBlockRegistrations, 1UZ)) << std::format("{} registered no block", library.file);
            }
            expect(that % loader.failedPlugins().empty()) << "a library that registered blocks is not a failed plugin";
            expect(that % gr::globalBlockRegistry().contains(kDoublerKey));
            expect(that % gr::globalBlockRegistry().contains(kTriplerKey));
        }

        std::unique_ptr<BlockModel> doubler = gr::globalBlockRegistry().create(kDoublerKey, {});
        std::unique_ptr<BlockModel> tripler = gr::globalBlockRegistry().create(kTriplerKey, {});
        expect(fatal(doubler != nullptr)) << "the first library's factory produced nothing";
        expect(fatal(tripler != nullptr)) << "the second library's factory produced nothing";

        doubler->settings().init();
        tripler->settings().init();
        expect(eq(std::string(doubler->typeName()), std::string("gr::testing::LibraryDoubler")));
        expect(eq(std::string(tripler->typeName()), std::string("gr::testing::LibraryTripler")));
        expect(eq(doubler->dynamicInputPorts().size(), 1UZ));
        expect(eq(tripler->dynamicOutputPorts().size(), 1UZ));
        expect(that % doubler->settings().defaultParameters().contains("extra_gain"));
    };

    "a library whose registrations replace another library's is kept mapped too"_test = [] {
        {
            const std::vector<std::string> directories{duplicateLibraryDirectory()};
            PluginLoader                   loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);

            expect(fatal(eq(loader.blockLibraries().size(), 2UZ))) << "a registration that replaces an entry counts as much as one that adds an entry";
            for (const PluginLoader::BlockLibrary& library : loader.blockLibraries()) {
                expect(ge(library.nBlockRegistrations, 1UZ)) << std::format("{} registered no block", library.file);
            }
            expect(that % loader.failedPlugins().empty()) << "a library that registered a block is not a failed plugin";
            expect(that % gr::globalBlockRegistry().contains(kDuplicateKey));
        }

        std::unique_ptr<BlockModel> duplicate = gr::globalBlockRegistry().create(kDuplicateKey, {});
        expect(fatal(duplicate != nullptr)) << "the surviving factory produced nothing";

        duplicate->settings().init();
        expect(eq(std::string(duplicate->typeName()), std::string("gr::testing::LibraryDuplicate")));
        expect(eq(duplicate->dynamicInputPorts().size(), 1UZ));
        expect(eq(duplicate->dynamicOutputPorts().size(), 1UZ));
    };

    "a plugin that registers nothing is closed and reported as before"_test = [] {
        const std::vector<std::string> directories{pluginDirectory()};
        PluginLoader                   loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);

        expect(that % loader.blockLibraries().empty()) << "no plugin fixture registers into the global registry";
        const auto failed = std::ranges::find_if(loader.failedPlugins(), [](const auto& entry) { return entry.first.contains("bad_plugin"); });
        expect(fatal(failed != loader.failedPlugins().end())) << "the plugin that cannot be instantiated is still a failure";
        expect(!failed->second.empty()) << "a failure carries its reason";
    };

    "a name that reads as a shared object but is not opened is reported"_test = [] {
        const std::vector<std::string> directories{skippedNamesDirectory()};
        PluginLoader                   loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);

        expect(fatal(eq(loader.skippedFiles().size(), 1UZ))) << "a soname-style name is skipped and reported";
        expect(loader.skippedFiles().front().contains("libnotopened.so.1"));
        expect(that % loader.blockLibraries().empty());
        expect(that % loader.failedPlugins().empty()) << "a file that was never opened did not fail to load";
    };
};

int main() { /* not needed for UT */ }
