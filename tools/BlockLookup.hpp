#ifndef GNURADIO_TOOLS_BLOCKLOOKUP_HPP
#define GNURADIO_TOOLS_BLOCKLOOKUP_HPP

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace gr::tools {

/// one directory a tool searches for blocks, and whether it is a directory at all
struct Directory {
    std::string path;
    std::string origin; // option, environment, or installation
    bool        present = false;
};

/**
 * @brief The directories a tool searches for blocks, in the order it searches them.
 *
 * The command line comes first, then the colon-separated list in GNURADIO4_PLUGIN_DIRECTORIES,
 * then `installed`, which is the plugin directory of the installation the caller was built for and
 * is always searched. A directory named twice is searched once.
 */
[[nodiscard]] inline std::vector<Directory> searchDirectories(std::span<const std::string> fromCommandLine, std::string_view installed) {
    std::vector<Directory> directories;
    auto                   add = [&directories](std::string_view path, std::string_view origin) {
        if (path.empty() || std::ranges::any_of(directories, [path](const Directory& held) { return held.path == path; })) {
            return;
        }
        std::error_code ignored;
        directories.push_back({.path = std::string(path), .origin = std::string(origin), .present = std::filesystem::is_directory(path, ignored)});
    };
    for (const std::string& directory : fromCommandLine) {
        add(directory, "option");
    }
    if (const char* environment = std::getenv("GNURADIO4_PLUGIN_DIRECTORIES"); environment != nullptr) {
        const std::string_view list(environment);
        for (std::size_t start = 0UZ; start < list.size();) {
            const std::size_t separator = list.find(':', start);
            const std::size_t end       = separator == std::string_view::npos ? list.size() : separator;
            add(list.substr(start, end - start), "environment");
            start = end + 1UZ;
        }
    }
    add(installed, "installation");
    return directories;
}

} // namespace gr::tools

#endif // GNURADIO_TOOLS_BLOCKLOOKUP_HPP
