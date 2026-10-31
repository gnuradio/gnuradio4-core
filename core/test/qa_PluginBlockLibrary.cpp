#include <boost/ut.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>

#include "MappedLibrary.hpp"
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

[[nodiscard]] std::string ordinaryLibraryDirectory() { return std::string(TESTS_BINARY_PATH) + "/ordinary_library"; }

[[nodiscard]] bool namedIn(std::string_view file, std::string_view name) { return std::filesystem::path(file).filename().string().contains(name); }

using gr::testing::isMapped;

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

    "a plugin that yields no instance is reported among the failed plugins and stays mapped"_test = [] {
        const std::vector<std::string> directories{pluginDirectory()};
        PluginLoader                   loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);

        expect(that % loader.blockLibraries().empty()) << "no plugin fixture registers into the global registry";
        const auto failed = std::ranges::find_if(loader.failedPlugins(), [](const auto& entry) { return entry.first.contains("bad_plugin"); });
        expect(fatal(failed != loader.failedPlugins().end())) << "the plugin that cannot be instantiated is still a failure";
        expect(!failed->second.empty()) << "a failure carries its reason";
        expect(that % isMapped(failed->first)) << "the loader unmapped a plugin that yields no instance";
    };

    "a file the dynamic linker cannot map is reported with the linker's reason"_test = [] {
        const std::filesystem::path directory = std::filesystem::path(TESTS_BINARY_PATH) / "unmappable_library";
        std::filesystem::create_directories(directory);
        const std::filesystem::path file = directory / "libempty.so";
        { std::ofstream(file).flush(); } // an empty file: dlopen refuses it and dlerror() says why
        expect(fatal(std::filesystem::exists(file)));

        const std::vector<std::string> directories{directory.string()};
        PluginLoader                   loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);
        std::filesystem::remove_all(directory);

        const auto failed = std::ranges::find_if(loader.failedPlugins(), [](const auto& entry) { return entry.first.contains("libempty.so"); });
        expect(fatal(failed != loader.failedPlugins().end())) << "a file dlopen refuses is a failed plugin";
        constexpr std::string_view prefix = "Failed to load the plugin file: ";
        expect(failed->second.starts_with(prefix)) << failed->second;
        expect(gt(failed->second.size(), prefix.size())) << "the reason names what the linker refused";
    };

    "a shared object that is neither a plugin nor a block library is listed apart from the failed plugins"_test = [] {
        const std::vector<std::string> directories{ordinaryLibraryDirectory()};
        const PluginLoader             loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);

        expect(fatal(eq(loader.ordinaryLibraries().size(), 1UZ))) << "the directory holds one shared object that is not a plugin";
        const std::string& ordinary = loader.ordinaryLibraries().front();
        expect(namedIn(ordinary, "ordinary_library")) << ordinary;
        expect(that % !loader.failedPlugins().contains(ordinary)) << "a library that is not a plugin did not fail to load";
        expect(that % isMapped(ordinary)) << "the loader unmapped a library it listed";
        expect(that % loader.blockLibraries().empty()) << "the library registered nothing";

        const PluginLoader again(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);
        expect(that % (again.ordinaryLibraries() == loader.ordinaryLibraries())) << "a later loader lists the library the same way";
        expect(that % !again.failedPlugins().contains(ordinary));
    };

    "a plugin without gr_plugin_free stays a failed plugin beside a library that is not a plugin"_test = [] {
        const std::vector<std::string> directories{ordinaryLibraryDirectory()};
        const PluginLoader             loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);

        const auto failed = std::ranges::find_if(loader.failedPlugins(), [](const auto& entry) { return namedIn(entry.first, "entry_point_only_plugin"); });
        expect(fatal(failed != loader.failedPlugins().end())) << "a file with gr_plugin_make and no gr_plugin_free is a failure";
        expect(eq(failed->second, std::string("Failed to load symbol gr_plugin_free")));
        expect(eq(loader.failedPlugins().size(), 1UZ)) << "no other file of the directory fails";
        expect(that % !std::ranges::any_of(loader.ordinaryLibraries(), [](const std::string& file) { return namedIn(file, "entry_point_only_plugin"); }));
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
