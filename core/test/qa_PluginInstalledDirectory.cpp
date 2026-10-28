#include <boost/ut.hpp>

#include <filesystem>
#include <string>
#include <string_view>

#include <gnuradio-4.0/PluginLoader.hpp>

#include "build_configure.hpp"

const boost::ut::suite<"PluginInstalledDirectory"> installedDirectoryTests = [] {
    using namespace boost::ut;

    "the installed plugin directory is the one the build configured"_test = [] {
        const std::string_view directory = gr::installedPluginDirectory();

        expect(eq(directory, std::string_view(GR_TEST_INSTALLED_PLUGIN_DIRECTORY)));
        expect(that % std::filesystem::path(directory).is_absolute()) << "the path has to hold for a program started from any directory";
        expect(eq(std::filesystem::path(directory).filename().string(), std::string("plugins")));
        expect(eq(std::filesystem::path(directory).parent_path().filename().string(), std::string("gnuradio-4")));
    };
};

int main() { /* not needed for UT */ }
