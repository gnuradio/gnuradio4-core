#include <boost/ut.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include <gnuradio-4.0/PluginLoader.hpp>

#include "build_configure.hpp"

/**
 * This program compiles its own copy of core/src/PluginLoader.cpp with the installed plugin directory set to the
 * directory of the test plugins, so that the process's loader has a directory to find without an installation.
 * The case covers the default branch of globalPluginLoader(). The value the library compiles in is covered by
 * qa_PluginInstalledDirectory.
 */
const boost::ut::suite<"PluginGlobalDefault"> globalDefaultTests = [] {
    using namespace boost::ut;

    "the process's loader searches the installed plugin directory when GNURADIO4_PLUGIN_DIRECTORIES is not set"_test = [] {
        expect(that % (std::getenv("GNURADIO4_PLUGIN_DIRECTORIES") == nullptr)) << "the test needs the variable unset";
        expect(that % !std::filesystem::exists("core/test/plugins")) << "the test needs a directory where the relative path finds nothing";
        expect(eq(gr::installedPluginDirectory(), std::string_view(TESTS_BINARY_PATH "/plugins")));

        const gr::PluginLoader& loader = gr::globalPluginLoader();
        expect(that % !loader.plugins().empty()) << "no plugin was loaded from the installed directory";
        expect(fatal(loader.isBlockAvailable("good::fixed_source<float64>"))) << "the block of a plugin in the installed directory is not offered";
    };
};

int main() { /* not needed for UT */ }
