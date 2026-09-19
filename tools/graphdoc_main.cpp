#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/config.hpp>

#include "DocOptions.hpp"
#include "GraphDoc.hpp"

#ifdef GR_ENABLE_BLOCK_REGISTRY
#include "BlockLookup.hpp"
#endif

namespace {

constexpr std::string_view kUsage = R"(graphdoc - describe a GNU Radio 4 flowgraph as Markdown or HTML

Usage: graphdoc [options] <graph.yaml>

  --format md|html   output format (default: md)
  --output <file>    write the document to <file> (default: standard output)
  --title <text>     document title (default: the name of the input file)
  --plugin-dir <dir> a directory of plugins and block libraries; repeatable
  --help, -h         this text

A lone - as the input reads the graph from standard input; the document is then
titled "standard input" unless --title names a title.

Reads the GRC YAML dialect the framework's own importer reads: blocks with their
parameters, connections, and SUBGRAPH entries with their nested graphs and
exported ports, to any depth. Every graph level gets a diagram, a table of
blocks and a table of connections, in that order, under a summary of the file.
A key the importer does not read is listed rather than dropped.

The connection table names the type each connection carries. That one column
comes from the blocks themselves: the tool constructs the source block of a
connection and reads the type off the named output port. Blocks come from the
registry linked into this program, from the directories named by --plugin-dir,
from the colon-separated list in GNURADIO4_PLUGIN_DIRECTORIES, and from the
plugin directory of this installation, which is always searched. A block no
registry holds and a port a block does not declare leave the column blank.
Everything else in the document is read from the file alone, so a graph whose
blocks this build does not provide is still described.

The HTML is a single self-contained page: its style sheet is embedded, it loads
nothing over the network, and it draws every diagram itself as an inline SVG.
The Markdown carries the same diagram as a mermaid flowchart.

Exit status is 0; 1 when the input is not the dialect or a directory named by
--plugin-dir cannot be searched, and 2 when the command line cannot be used, the
input cannot be read or the output cannot be written.
)";

} // namespace

int main(int argc, char** argv) {
    using namespace gr::tools;

    const std::vector<std::string_view> arguments = argumentsOf(argc, argv);
    CommonOptions                       options;
    std::vector<std::string>            pluginDirectories;
    std::string                         inputPath;
    std::string                         error;

    for (std::size_t index = 0UZ; index < arguments.size();) {
        switch (readCommonOption(arguments, index, options, error)) {
        case OptionResult::Taken: continue;
        case OptionResult::Failed: std::println(stderr, "graphdoc: {}", error); return 2;
        case OptionResult::NotMine: break;
        }
        const std::string_view argument = arguments[index];
        if (argument == "--plugin-dir") {
            if (index + 1UZ >= arguments.size()) {
                std::println(stderr, "graphdoc: --plugin-dir needs a value");
                return 2;
            }
            pluginDirectories.emplace_back(arguments[index + 1UZ]);
            index += 2UZ;
            continue;
        }
        if (argument.starts_with("-") && argument != "-") {
            std::println(stderr, "graphdoc: unknown option '{}'", argument);
            return 2;
        }
        if (!inputPath.empty()) {
            std::println(stderr, "graphdoc: more than one input file given ('{}' and '{}')", inputPath, argument);
            return 2;
        }
        inputPath.assign(argument);
        ++index;
    }

    if (options.help) {
        std::print("{}", kUsage);
        return 0;
    }
    if (inputPath.empty()) {
        std::println(stderr, "graphdoc: no input file given; --help describes the options");
        return 2;
    }

#ifdef GR_ENABLE_BLOCK_REGISTRY
    const std::vector<Directory> directories = searchDirectories(pluginDirectories, GR_TOOLS_INSTALLED_PLUGIN_DIRECTORY);
    std::vector<std::string>     paths;
    bool                         searchable = true;
    for (const Directory& directory : directories) {
        if (directory.origin == "option" && !directory.present) {
            std::println(stderr, "graphdoc: {} could not be searched; it is not a directory", directory.path);
            searchable = false;
        }
        paths.push_back(directory.path);
    }
    if (!searchable) {
        return 1;
    }
#else
    if (!pluginDirectories.empty()) {
        std::println(stderr, "graphdoc: this build carries no block registry, so --plugin-dir resolves nothing");
    }
#endif

    const bool         fromStandardInput = inputPath == "-";
    std::ostringstream contents;
    if (fromStandardInput) {
        contents << std::cin.rdbuf();
    } else {
        std::ifstream input(inputPath, std::ios::binary);
        if (!input) {
            std::println(stderr, "graphdoc: cannot read '{}'", inputPath);
            return 2;
        }
        contents << input.rdbuf();
    }

    auto level = graphdoc::read(contents.str());
    if (!level.has_value()) {
        std::println(stderr, "graphdoc: '{}' is not a valid graph document: {}", inputPath, level.error());
        return 1;
    }

#ifdef GR_ENABLE_BLOCK_REGISTRY
    gr::PluginLoader loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), paths);
    OutputPortTypes  outputPortTypes(loader);
    graphdoc::resolveConnectionTypes(*level, [&outputPortTypes](std::string_view blockType, std::string_view port) { return outputPortTypes(blockType, port); });
#endif

    if (options.title.empty()) {
        options.title = fromStandardInput ? std::string("standard input") : std::filesystem::path(inputPath).filename().string();
    }

    const auto written = writeDocument(options.output, graphdoc::render(*level, options.format, options.title));
    if (!written.has_value()) {
        std::println(stderr, "graphdoc: {}", written.error());
        return 2;
    }
    return 0;
}
