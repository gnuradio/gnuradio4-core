#include <boost/ut.hpp>

#include <cstdlib>
#include <filesystem>
#include <format>
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

    "every shared plugin directory exists and is keyed on the plugin ABI version"_test = [] {
        const std::string leaf = std::format("v{}", GR_PLUGIN_CURRENT_ABI_VERSION);
        for (const std::string& directory : gr::sharedPluginDirectories()) {
            expect(that % std::filesystem::is_directory(directory)) << directory;
            expect(eq(std::filesystem::path(directory).filename().string(), leaf)) << directory;
        }
    };

#ifdef _WIN32
    "a per-user install's shared plugin directory is searched"_test = [] {
        const std::filesystem::path root      = std::filesystem::temp_directory_path() / "gr4_qa_shared_plugins";
        const std::filesystem::path directory = root / ".gnuradio-4" / "plugins" / std::format("v{}", GR_PLUGIN_CURRENT_ABI_VERSION);
        std::filesystem::create_directories(directory);
        expect(fatal(::_putenv_s("LOCALAPPDATA", root.string().c_str()) == 0));

        const std::vector<std::string> directories = gr::sharedPluginDirectories();
        expect(fatal(!directories.empty()));
        expect(std::filesystem::path(directories.front()) == directory);
        std::filesystem::remove_all(root);
    };
#endif
};

int main() { /* not needed for UT */ }
