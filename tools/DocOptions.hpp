#ifndef GNURADIO_TOOLS_DOCOPTIONS_HPP
#define GNURADIO_TOOLS_DOCOPTIONS_HPP

#include <cstdio>
#include <expected>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "DocWriter.hpp"

namespace gr::tools {

/// the options that describe the document itself; a tool adds its own on top
struct CommonOptions {
    Format      format = Format::Markdown;
    std::string output; ///< empty writes to stdout
    std::string title;
    bool        help = false;
};

/// How far an argument got: `NotMine` leaves it to the caller's own option table.
enum class OptionResult { NotMine, Taken, Failed };

/**
 * @brief Reads one of the shared options at `index`, advancing it past the words consumed.
 *
 * An option that needs a value and has none is a failure with the reason in `error`, so a
 * caller never sees a half-read option or a value silently defaulted.
 */
inline OptionResult readCommonOption(std::span<const std::string_view> args, std::size_t& index, CommonOptions& options, std::string& error) {
    const std::string_view argument = args[index];

    auto valueOf = [&](std::string_view name) -> const std::string_view* {
        if (index + 1UZ >= args.size()) {
            error = std::format("{} needs a value", name);
            return nullptr;
        }
        ++index;
        return &args[index];
    };

    if (argument == "--help" || argument == "-h") {
        options.help = true;
        ++index;
        return OptionResult::Taken;
    }
    if (argument == "--format") {
        const std::string_view* value = valueOf(argument);
        if (value == nullptr) {
            return OptionResult::Failed;
        }
        const auto parsed = parseFormat(*value);
        if (!parsed.has_value()) {
            error = std::format("unknown format '{}', expected md or html", *value);
            return OptionResult::Failed;
        }
        options.format = *parsed;
        ++index;
        return OptionResult::Taken;
    }
    if (argument == "--output") {
        const std::string_view* value = valueOf(argument);
        if (value == nullptr) {
            return OptionResult::Failed;
        }
        options.output.assign(*value);
        ++index;
        return OptionResult::Taken;
    }
    if (argument == "--title") {
        const std::string_view* value = valueOf(argument);
        if (value == nullptr) {
            return OptionResult::Failed;
        }
        options.title.assign(*value);
        ++index;
        return OptionResult::Taken;
    }
    return OptionResult::NotMine;
}

[[nodiscard]] inline std::vector<std::string_view> argumentsOf(int argc, char** argv) {
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        arguments.emplace_back(argv[i]);
    }
    return arguments;
}

/// writes the finished document, to `path` or to stdout when it is empty
[[nodiscard]] inline std::expected<void, std::string> writeDocument(const std::string& path, std::string_view document) {
    if (path.empty()) {
        std::fwrite(document.data(), 1UZ, document.size(), stdout);
        return {};
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::unexpected(std::format("cannot write '{}'", path));
    }
    out.write(document.data(), static_cast<std::streamsize>(document.size()));
    if (!out) {
        return std::unexpected(std::format("write to '{}' failed", path));
    }
    return {};
}

} // namespace gr::tools

#endif // GNURADIO_TOOLS_DOCOPTIONS_HPP
