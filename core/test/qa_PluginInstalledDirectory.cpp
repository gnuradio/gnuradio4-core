#include <boost/ut.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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

    "GNURADIO4_PLUGIN_DIRECTORIES splits on the platform's path-list separator and drops empty entries"_test = [] {
        const std::string separator(1UZ, gr::kPluginDirectorySeparator);
#ifdef _WIN32
        expect(eq(gr::kPluginDirectorySeparator, ';'));
        const std::string first  = R"(C:\gr\plugins)";
        const std::string second = R"(D:\more plugins)";
#else
        expect(eq(gr::kPluginDirectorySeparator, ':'));
        const std::string first  = "/opt/gr/plugins";
        const std::string second = "/home/user/more plugins";
#endif
        expect(gr::splitPluginDirectories("").empty());
        expect(gr::splitPluginDirectories(separator + separator).empty());
        expect(gr::splitPluginDirectories(first) == std::vector<std::string>{first}) << "a drive letter is not a separator";
        expect(gr::splitPluginDirectories(separator + first + separator + separator + second + separator) == std::vector<std::string>{first, second});
    };
};

int main() { /* not needed for UT */ }
