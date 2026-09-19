#ifndef GNURADIO_TOOLS_BLOCKLOOKUP_HPP
#define GNURADIO_TOOLS_BLOCKLOOKUP_HPP

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <gnuradio-4.0/BlockModel.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Settings.hpp>

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

/// one output port of a block, as the block declares it
struct OutputPort {
    std::string name;
    std::string dataType;
};

/**
 * @brief The type of the items a block's output port carries, read from an instance of the block.
 *
 * A registry key is all a graph file gives, and only a block itself knows what its ports carry, so
 * the answer comes from a default-constructed instance: `settings().init()` copies the annotations
 * into the meta information and fills the port descriptions, and the block is never started. A key
 * no registry holds, a factory that refuses, and a port name the block does not declare each answer
 * with an empty string. The caller then prints no type at all, never a guess.
 *
 * Every key is read once and kept, because a graph names the same block on many connections.
 */
class OutputPortTypes {
    PluginLoader&                                               _loader;
    std::map<std::string, std::vector<OutputPort>, std::less<>> _byKey;

public:
    explicit OutputPortTypes(PluginLoader& loader) : _loader(loader) {}

    /// `port` is the port as a connection spells it: its name, its name and index within a
    /// collection, or its position among the block's output ports with an optional sub-index
    [[nodiscard]] std::string operator()(std::string_view blockKey, std::string_view port) {
        const std::vector<OutputPort>& ports = outputsOf(blockKey);
        if (ports.empty()) {
            return {};
        }

        const std::string_view index = port.substr(0UZ, port.find('.'));
        std::size_t            at    = 0UZ;
        if (const auto [after, failed] = std::from_chars(index.data(), index.data() + index.size(), at); failed == std::errc{} && after == index.data() + index.size()) {
            return at < ports.size() ? ports[at].dataType : std::string{};
        }

        const std::string_view name  = port.substr(0UZ, port.find('#'));
        const auto             named = std::ranges::find(ports, name, &OutputPort::name);
        return named == ports.end() ? std::string{} : named->dataType;
    }

private:
    const std::vector<OutputPort>& outputsOf(std::string_view blockKey) {
        if (const auto held = _byKey.find(blockKey); held != _byKey.end()) {
            return held->second;
        }

        std::shared_ptr<BlockModel> instance;
        try {
            instance = _loader.instantiate(blockKey, {});
        } catch (...) {
            instance = nullptr; // a factory that throws leaves every connection out of it blank
        }

        std::vector<OutputPort> ports;
        if (instance != nullptr) {
            try {
                instance->settings().init();
            } catch (...) {
                // a block that refuses its own defaults still declares its ports
            }
            // a collection's members all carry the type of the collection, so it enters as one entry
            for (auto& entry : instance->dynamicOutputPorts()) {
                if (auto* collection = std::get_if<BlockModel::NamedPortCollection>(&entry); collection != nullptr) {
                    ports.push_back({.name = std::string(collection->name), .dataType = collection->ports.empty() ? std::string{} : std::string(collection->ports.front().metaInfo.data_type)});
                } else if (auto* single = std::get_if<DynamicPort>(&entry); single != nullptr) {
                    ports.push_back({.name = std::string(single->metaInfo.name), .dataType = std::string(single->metaInfo.data_type)});
                }
            }
        }
        return _byKey.emplace(std::string(blockKey), std::move(ports)).first->second;
    }
};

} // namespace gr::tools

#endif // GNURADIO_TOOLS_BLOCKLOOKUP_HPP
