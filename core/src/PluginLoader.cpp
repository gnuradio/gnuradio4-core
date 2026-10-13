#include <gnuradio-4.0/PluginLoader.hpp>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace gr {
namespace {

// Why `directory` cannot serve as a writable directory, or nothing when it can. An existing directory needs write and
// search permission; a missing one needs them on its nearest existing ancestor, under which it would be made. A broken
// symbolic link in the missing part is refused, since a directory cannot be made in its place.
std::optional<std::string> whyNotWritable(const std::filesystem::path& directory) {
    std::filesystem::path probe = directory;
    for (;;) {
        std::error_code                    ec;
        const std::filesystem::file_status status = std::filesystem::status(probe, ec);
        if (status.type() == std::filesystem::file_type::not_found) {
            std::error_code linkEc;
            if (std::filesystem::symlink_status(probe, linkEc).type() != std::filesystem::file_type::not_found && !linkEc) {
                return std::format("{} is a broken symbolic link", probe.string());
            }
            std::filesystem::path parent = probe.parent_path();
            if (parent.empty() || parent == probe) {
                return std::format("{} has no existing ancestor", directory.string());
            }
            probe = std::move(parent);
            continue;
        }
        if (ec) {
            return std::format("{}: {}", probe.string(), ec.message());
        }
        if (!std::filesystem::is_directory(status)) {
            return std::format("{} is not a directory", probe.string());
        }
#ifdef _WIN32
        const bool writable = ::_access(probe.string().c_str(), 2) == 0;
#else
        const bool writable = ::access(probe.c_str(), W_OK | X_OK) == 0;
#endif
        if (!writable) {
            return std::format("{}: {}", probe.string(), std::generic_category().message(errno));
        }
        return std::nullopt;
    }
}

} // namespace

DataCacheDirectory dataCacheDirectory(std::string_view compiled) {
    DataCacheDirectory chosen{.path = {}, .origin = DataCacheDirectory::Origin::None, .compiled = std::string(compiled), .whyNotCompiled = {}, .user = {}, .whyNotUser = {}};
    if (const char* environment = std::getenv("GR_DATA_CACHE_DIR"); environment != nullptr && *environment != '\0') {
        chosen.path   = environment;
        chosen.origin = DataCacheDirectory::Origin::Environment;
        return chosen;
    }
    const std::optional<std::string> compiledRefused = whyNotWritable(chosen.compiled);
    if (!compiledRefused.has_value()) {
        chosen.path   = chosen.compiled;
        chosen.origin = DataCacheDirectory::Origin::Compiled;
        return chosen;
    }
    chosen.whyNotCompiled = *compiledRefused;

    std::filesystem::path user;
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && std::filesystem::path(xdg).is_absolute()) {
        user = std::filesystem::path(xdg) / "gnuradio4" / "cache";
    } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        user = std::filesystem::path(home) / ".cache" / "gnuradio4" / "cache";
    } else {
        chosen.whyNotUser = "neither XDG_CACHE_HOME nor HOME names a user's cache directory";
        return chosen;
    }
    chosen.user = user.string();
    if (const std::optional<std::string> userRefused = whyNotWritable(user); userRefused.has_value()) {
        chosen.whyNotUser = *userRefused;
        return chosen;
    }
    chosen.path   = chosen.user;
    chosen.origin = DataCacheDirectory::Origin::User;
    return chosen;
}

PluginLoader& globalPluginLoader() {
    auto pluginPaths = [] {
        std::vector<std::string> result;

        auto* envpath = ::getenv("GNURADIO4_PLUGIN_DIRECTORIES");
        if (envpath == nullptr) {
            // TODO choose proper paths when we get the system GR installation done
            result.emplace_back("core/test/plugins");

        } else {
            std::string_view paths(envpath);

            auto i = paths.cbegin();

            // TODO If we want to support Windows, this should be ; there
            auto isSeparator = [](char c) { return c == ':'; };

            while (i != paths.cend()) {
                i      = std::find_if_not(i, paths.cend(), isSeparator);
                auto j = std::find_if(i, paths.cend(), isSeparator);

                if (i != paths.cend()) {
                    result.emplace_back(std::string_view(i, j));
                }
                i = j;
            }
        }

        return result;
    };

    static PluginLoader instance(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), {pluginPaths()});
    return instance;
}
} // namespace gr
