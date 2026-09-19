#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "DocOptions.hpp"
#include "GraphDoc.hpp"

namespace {

constexpr std::string_view kUsage = R"(graphdoc - describe a GNU Radio 4 flowgraph as Markdown or HTML

Usage: graphdoc [options] <graph.yaml>

  --format md|html   output format (default: md)
  --output <file>    write the document to <file> (default: standard output)
  --title <text>     document title (default: the name of the input file)
  --help, -h         this text

A lone - as the input reads the graph from standard input; the document is then
titled "standard input" unless --title names a title.

Reads the GRC YAML dialect the framework's own importer reads: blocks with their
parameters, connections, and SUBGRAPH entries with their nested graphs and
exported ports, to any depth. Every graph level gets a diagram, a table of
blocks and a table of connections, in that order, under a summary of the file.
A key the importer does not read is listed rather than dropped.

The whole document is read from the file alone, so a graph whose blocks this
build does not provide is described just as well.

The HTML is a single self-contained page: its style sheet is embedded, it loads
nothing over the network, and it draws every diagram itself as an inline SVG.
The Markdown carries the same diagram as a mermaid flowchart.

Exit status is 0; 1 when the input is not the dialect, and 2 when the command
line cannot be used, the input cannot be read or the output cannot be written.
)";

} // namespace

int main(int argc, char** argv) {
    using namespace gr::tools;

    const std::vector<std::string_view> arguments = argumentsOf(argc, argv);
    CommonOptions                       options;
    std::string                         inputPath;
    std::string                         error;

    for (std::size_t index = 0UZ; index < arguments.size();) {
        switch (readCommonOption(arguments, index, options, error)) {
        case OptionResult::Taken: continue;
        case OptionResult::Failed: std::println(stderr, "graphdoc: {}", error); return 2;
        case OptionResult::NotMine: break;
        }
        const std::string_view argument = arguments[index];
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

    const auto level = graphdoc::read(contents.str());
    if (!level.has_value()) {
        std::println(stderr, "graphdoc: '{}' is not a valid graph document: {}", inputPath, level.error());
        return 1;
    }

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
