#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <cstdio>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include <gnuradio-4.0/config.hpp>

#include "GraphDoc.hpp"

#ifdef GR_ENABLE_BLOCK_REGISTRY
#include <complex>
#include <tuple>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>

#include "BlockLookup.hpp"
#endif

/**
 * The flowgraph document: what the reader must be able to find in it.
 *
 * The two pinned documents in `assets/` are compared as whole files by ctest, which catches any
 * change to the layout. The assertions here are the ones a diff cannot make: that every block,
 * every connection and every level of the fixture reached the document at all, that the nesting
 * put them in the right section, and that the HTML page stands alone.
 */
namespace qa_graphdoc {

using namespace gr::tools;

[[nodiscard]] std::string fixture() {
    std::ifstream      input(std::string(GR_TOOLS_TEST_ASSETS) + "/nested_graph.yaml", std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

[[nodiscard]] std::size_t occurrences(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0UZ;
    for (std::size_t at = haystack.find(needle); at != std::string_view::npos; at = haystack.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

/// the `x` the element beginning at `at` is drawn at, or -1 where it states none
[[nodiscard]] double leftEdgeOf(std::string_view document, std::size_t at) {
    const std::size_t attribute = document.find(" x=\"", at);
    if (attribute == std::string_view::npos) {
        return -1.0;
    }
    const std::size_t begin = attribute + 4UZ;
    return std::stod(std::string(document.substr(begin, document.find('"', begin) - begin)));
}

[[nodiscard]] graphdoc::Level readFixture() {
    auto level = graphdoc::read(fixture());
    if (!level.has_value()) {
        throw std::runtime_error(level.error());
    }
    return std::move(*level);
}

#ifdef GR_ENABLE_BLOCK_REGISTRY
/// An output whose type is neither the input's nor anything the registry key spells, so a
/// connection type that comes out right can only have been read from the block.
struct Widener : gr::Block<Widener> {
    gr::PortIn<float>                in;
    gr::PortOut<std::complex<float>> out;

    GR_MAKE_REFLECTABLE(Widener, in, out);

    [[nodiscard]] constexpr std::complex<float> processOne(float value) const noexcept { return {value, 0.0f}; }
};

void registerTestBlocks() {
    static const bool registered = [] {
        std::ignore = gr::globalBlockRegistry().insert<Widener>();
        return true;
    }();
    std::ignore = registered;
}

/// The importer's verdict on a document: true where `gr::loadGrc` builds a graph from it. A case
/// compares this verdict with the tool's and leaves the two messages alone. The importer reports
/// several document shapes with the message of a standard library container, and the tool names
/// the field.
[[nodiscard]] bool importerAccepts(std::string_view yaml) {
    const std::vector<std::string> noDirectories;
    gr::PluginLoader               loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), noDirectories);
    try {
        [[maybe_unused]] const auto graph = gr::loadGrc(loader, yaml);
        return true;
    } catch (...) {
        return false;
    }
}
#endif

#ifdef _WIN32
constexpr auto openPipe  = _popen;
constexpr auto closePipe = _pclose;
#else
constexpr auto openPipe  = popen;
constexpr auto closePipe = pclose;
#endif

struct Run {
    int         exitCode = -1;
    std::string output; // standard output and standard error together, in the order the run wrote them
};

/// Writes `yaml` to a file of the test's own and describes it with the built program. A refusal is
/// the status the command exits with and the line it prints, and a test reads neither from inside
/// this process.
[[nodiscard]] Run describe(std::string_view fileName, std::string_view yaml) {
    const std::string path = std::format("{}/{}", GR_TOOLS_TEST_SCRATCH, fileName);
    {
        std::ofstream file(path, std::ios::binary);
        file << yaml;
    }

    Run               result;
    const std::string command = std::format("\"{}\" --format md \"{}\" 2>&1", GR_TOOLS_GRAPHDOC, path);
    std::FILE*        pipe    = openPipe(command.c_str(), "r");
    if (pipe == nullptr) {
        return result;
    }
    std::array<char, 4096UZ> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result.output.append(buffer.data());
    }
    const int status = closePipe(pipe);
#ifdef _WIN32
    result.exitCode = status;
#else
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    return result;
}

struct Refusal {
    std::string_view fileName;
    std::string_view yaml;
    std::string_view message; ///< the sentence the tool gives for the file
};

/// One file per rule the importer enforces on a graph document's shape, each with the sentence the
/// tool gives for it. The importer refuses the same files. It reports several of these shapes with
/// the message of a standard library container, and a case compares the two verdicts rather than
/// the two messages.
constexpr std::array<Refusal, 21> kRefusals{{
    {"no_id.yaml", "blocks:\n  - parameters:\n      name: source\n", "Missing field id in YAML object"},
    {"id_not_a_string.yaml", "blocks:\n  - id: 42\n    parameters:\n      name: source\n", "Field id in YAML object has an incorrect type"},
    {"no_parameters.yaml", "blocks:\n  - id: qa::Scale\n", "Missing field parameters in YAML object"},
    {"parameters_not_a_map.yaml", "blocks:\n  - id: qa::Scale\n    parameters: 3\n", "Field parameters in YAML object has an incorrect type"},
    {"no_name.yaml", "blocks:\n  - id: qa::Scale\n    parameters:\n      gain: 2.0\n", "Missing field name in YAML object"},
    {"subgraph_without_graph.yaml", "blocks:\n  - id: SUBGRAPH\n    parameters:\n      name: inner\n", "Missing field graph in YAML object"},
    {"graph_not_a_map.yaml", "blocks:\n  - id: SUBGRAPH\n    parameters:\n      name: inner\n    graph: 7\n", "Unable to create block 'inner' of type 'SUBGRAPH': graph is not a map"},
    {"scheduler_not_a_map.yaml", "blocks:\n  - id: SUBGRAPH\n    parameters:\n      name: inner\n    scheduler: simple\n    graph:\n      blocks:\n        - id: qa::Scale\n          parameters:\n            name: gain\n", "scheduler is not a property_map"},
    {"scheduler_without_id.yaml", "blocks:\n  - id: SUBGRAPH\n    parameters:\n      name: inner\n    scheduler:\n      parameters:\n        thread_pool: workers\n    graph:\n      blocks:\n        - id: qa::Scale\n          parameters:\n            name: gain\n", "Missing field id in YAML object"},
    {"exported_port_not_a_list.yaml", "blocks:\n  - id: SUBGRAPH\n    parameters:\n      name: inner\n    graph:\n      blocks:\n        - id: qa::Scale\n          parameters:\n            name: gain\n      exported_ports:\n        - gain\n", "Unable to parse exported port (not a list)"},
    {"exported_port_of_three.yaml", "blocks:\n  - id: SUBGRAPH\n    parameters:\n      name: inner\n    graph:\n      blocks:\n        - id: qa::Scale\n          parameters:\n            name: gain\n      exported_ports:\n        - [gain, INPUT, in]\n", "Unable to parse exported port (3 instead of 4 elements)"},
    {"exported_port_field.yaml", "blocks:\n  - id: SUBGRAPH\n    parameters:\n      name: inner\n    graph:\n      blocks:\n        - id: qa::Scale\n          parameters:\n            name: gain\n      exported_ports:\n        - [gain, INPUT, in, 4]\n", "Required fields for exported ports missing"},
    {"contexts_not_a_list.yaml", "blocks:\n  - id: qa::Scale\n    parameters:\n      name: gain\n    ctx_parameters: 5\n", "Unable to create block 'gain' of type 'qa::Scale': ctx_parameters is not a list"},
    {"context_not_a_map.yaml", "blocks:\n  - id: qa::Scale\n    parameters:\n      name: gain\n    ctx_parameters:\n      - fast\n", "a ctx_parameters entry is not a map"},
    {"context_without_time.yaml", "blocks:\n  - id: qa::Scale\n    parameters:\n      name: gain\n    ctx_parameters:\n      - context: fast\n        parameters:\n          buffer_size: 512\n", "a ctx_parameters entry needs a context, a context_time and a parameters map"},
    {"connection_not_a_list.yaml", "blocks:\n  - id: qa::Scale\n    parameters:\n      name: gain\nconnections: [42]\n", "Unable to parse connection (not a list)"},
    {"connection_of_three.yaml", "blocks:\n  - id: qa::Scale\n    parameters:\n      name: gain\nconnections:\n  - [gain, out, gain]\n", "Unable to parse connection (3 instead of >=4 elements)"},
    {"connection_block_field.yaml", "blocks:\n  - id: qa::Scale\n    parameters:\n      name: gain\nconnections:\n  - [7, out, gain, in]\n", "Invalid blockField"},
    {"port_pair_of_three.yaml", "blocks:\n  - id: qa::Scale\n    parameters:\n      name: gain\nconnections:\n  - [gain, [0, 0, 0], gain, in]\n", "Port definition has invalid length (3 instead of 2)"},
    {"port_pair_not_indices.yaml", "blocks:\n  - id: qa::Scale\n    parameters:\n      name: gain\nconnections:\n  - [gain, [a, b], gain, in]\n", "Port definition missing values"},
    {"port_not_a_definition.yaml", "blocks:\n  - id: qa::Scale\n    parameters:\n      name: gain\nconnections:\n  - [gain, 1.5, gain, in]\n", "Port definition missing values"},
}};

/// The same key in the two places a document may carry it: a document's top level, which the
/// importer does not read, and the `graph` map of a SUBGRAPH entry, which it does. The entry has
/// the wrong shape in both places.
constexpr std::string_view kRootExportedPorts     = "exported_ports: [42]\n";
constexpr std::string_view kSubgraphExportedPorts = "blocks:\n  - id: SUBGRAPH\n    parameters:\n      name: inner\n    graph:\n      exported_ports: [42]\n";

/// a graph carrying every key the reader knows, a key it does not, and keys nested under both
constexpr std::string_view kEveryKey = R"(definition_metadata:
  plugin_name: Keys fixture
graph_version: 7
blocks:
  - id: qa::RampSource
    unique_name: source_1
    block_category: NormalBlock
    parameters:
      name: source
      n_samples: !!uint32 64
      nested:
        depth: leaf_value
    meta_information:
      role: origin
    ctx_parameters:
      - context: fast
        time: !!uint64 3
        parameters:
          buffer_size: 512
        note: context_extra
    layout_hint: top-left
  - id: SUBGRAPH
    block_category: ScheduledBlockGroup
    parameters:
      name: front_end
    scheduler:
      id: gr::scheduler::Simple
      parameters:
        thread_pool: workers
      affinity: cpu_two
    graph:
      definition_metadata:
        inner_note: nested_metadata
      blocks:
        - id: qa::Scale
          parameters:
            name: gain
      exported_ports:
        - [gain, INPUT, in, in]
connections:
  - [source_1, 0, front_end, in, 4096, extra_element]
)";

/// every scalar the file above carries, each of which the document has to hold
constexpr std::array<std::string_view, 22> kEveryKeyScalars{"Keys fixture", "7", "source_1", "NormalBlock", "source", "64", "leaf_value", "origin", "fast", "3", "512", "context_extra", "top-left", "ScheduledBlockGroup", "front_end", "gr::scheduler::Simple", "workers", "cpu_two", "nested_metadata", "gain", "4096", "extra_element"};

/// every block name and type, and both ends of every connection, at every level of the fixture
constexpr std::array<std::string_view, 7> kBlockNames{"source", "front_end", "sink", "pre_gain", "inner_chain", "fine_gain", "combiner"};
constexpr std::array<std::string_view, 4> kBlockTypes{"qa::RampSource", "qa::Scale", "qa::SumInputs", "qa::RecordingSink"};
constexpr std::array<std::string_view, 8> kConnectionEnds{"source_1", "front_end", "front_end", "sink", "pre_gain", "inner_chain", "fine_gain", "combiner"};

} // namespace qa_graphdoc

const boost::ut::suite<"GraphDoc"> graphDocTests = [] {
    using namespace boost::ut;
    using namespace gr::tools;
    using namespace qa_graphdoc;

    "the fixture parses into three graph levels"_test = [] {
        const graphdoc::Level level = readFixture();
        expect(eq(level.blocks.size(), 3UZ));
        expect(eq(level.connections.size(), 2UZ));
        expect(eq(graphdoc::countSubgraphs(level), 2UZ));
        expect(eq(level.metadata.size(), 5UZ)) << "definition_metadata carries five entries";
        expect(eq(level.uninterpretedKeys.size(), 1UZ)) << "graph_version is not a key the reader interprets";
    };

    "the document names every block and every connection"_test = [] {
        const std::string document = graphdoc::render(readFixture(), Format::Markdown, "Nested graph fixture");
        for (const std::string_view name : kBlockNames) {
            expect(document.find(name) != std::string::npos) << std::format("block '{}' is missing from the document", name);
        }
        for (const std::string_view type : kBlockTypes) {
            expect(document.find(type) != std::string::npos) << std::format("type '{}' is missing from the document", type);
        }
        for (const std::string_view endpoint : kConnectionEnds) {
            expect(document.find(endpoint) != std::string::npos) << std::format("connection endpoint '{}' is missing from the document", endpoint);
        }
        expect(document.find("4096") != std::string::npos) << "the minimum buffer size of the second connection is missing";
        expect(document.find("[context fast @ 0]") != std::string::npos) << "the context parameters of the sink are missing";
    };

    "each subgraph has its own section, exported ports and diagram"_test = [] {
        const std::string document = graphdoc::render(readFixture(), Format::Markdown, "Nested graph fixture");
        const std::size_t outer    = document.find("## Subgraph: front_end");
        const std::size_t inner    = document.find("### Subgraph: front_end / inner_chain");
        expect(outer != std::string::npos) << "the outer subgraph has no section";
        expect(inner != std::string::npos) << "the inner subgraph has no section of its own";
        expect(outer < inner) << "the inner section must nest inside the outer one";
        expect(document.find("fine_gain") > outer) << "an inner block is documented at the top level";
        expect(eq(occurrences(document, "Exported ports"), 2UZ)) << "both subgraphs export ports";
        expect(document.find("gr::scheduler::Simple") != std::string::npos) << "the scheduler of the managed subgraph is missing";
        expect(eq(occurrences(document, "```mermaid"), 3UZ)) << "one diagram per graph level";
    };

    "the block table has four columns, the unique name and the kind folded into two of them"_test = [] {
        const std::string markdown = graphdoc::render(readFixture(), Format::Markdown, "Nested graph fixture");
        expect(markdown.find("| Name | Type | Parameters | Meta information |") != std::string::npos) << "the table names four columns";
        expect(markdown.find("| Kind |") == std::string::npos) << "the kind has a column no longer";
        expect(markdown.find("| Unique name |") == std::string::npos) << "and neither has the unique name";
        expect(markdown.find("| source<br>(source_1) |") != std::string::npos) << "the unique name sits under the name";
        expect(markdown.find("SUBGRAPH<br>(subgraph)") != std::string::npos) << "a subgraph's kind sits under its type";
        expect(markdown.find("SUBGRAPH<br>(subgraph, scheduler gr::scheduler::Simple)") != std::string::npos) << "and names the scheduler that manages it";
        expect(markdown.find("| qa::RampSource |") != std::string::npos) << "a plain block's type cell carries the type alone";

        const std::string html = graphdoc::render(readFixture(), Format::Html, "Nested graph fixture");
        expect(html.find("<th>Name</th><th>Type</th><th>Parameters</th><th>Meta information</th>") != std::string::npos) << html;
        expect(html.find("<td>source<br>(source_1)</td>") != std::string::npos) << "the cell breaks its line in the page too";
    };

    "a templated type splits at its first angle bracket"_test = [] {
        const auto level = graphdoc::read("blocks:\n  - id: qa::Convert<float32, complex<float32>>\n    parameters:\n      name: widen\n");
        expect(level.has_value());
        const std::string markdown = graphdoc::render(*level, Format::Markdown, "t");
        expect(markdown.find("qa::Convert<br><float32, complex<float32>>") != std::string::npos) << markdown;

        const std::string html = graphdoc::render(*level, Format::Html, "t");
        expect(html.find("qa::Convert<br>&lt;float32, complex&lt;float32&gt;&gt;") != std::string::npos) << html;
    };

#ifdef GR_ENABLE_BLOCK_REGISTRY
    "the connection table names the type the source block's output port carries"_test = [] {
        registerTestBlocks();
        const std::vector<std::string> noDirectories;
        gr::PluginLoader               loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), noDirectories);
        OutputPortTypes                outputPortTypes(loader);

        const std::string key    = gr::meta::type_name<Widener>();
        const std::string source = std::format("blocks:\n"
                                               "  - id: {0}\n    parameters:\n      name: first\n"
                                               "  - id: {0}\n    parameters:\n      name: second\n"
                                               "  - id: qa::NoSuchBlockIsRegistered\n    parameters:\n      name: third\n"
                                               "connections:\n"
                                               "  - [first, out, second, in]\n"
                                               "  - [first, 0, third, in]\n"
                                               "  - [third, out, second, in]\n"
                                               "  - [second, no_such_port, third, in]\n",
            key);
        auto              level  = graphdoc::read(source);
        expect(fatal(level.has_value()));
        graphdoc::resolveConnectionTypes(*level, [&outputPortTypes](std::string_view blockType, std::string_view port) { return outputPortTypes(blockType, port); });

        expect(eq(level->connections[0].itemType, std::string("complex<float32>"))) << "the output port's type, not the input's and not the key's";
        expect(eq(level->connections[1].itemType, std::string("complex<float32>"))) << "a port named by its position resolves as well";
        expect(level->connections[2].itemType.empty()) << "a block no registry holds leaves the column blank";
        expect(level->connections[3].itemType.empty()) << "a port the block does not declare leaves it blank too";

        const std::string markdown = graphdoc::render(*level, Format::Markdown, "t");
        expect(markdown.find("| From | Port | To | Port | Type | Minimum buffer |") != std::string::npos) << markdown;
        expect(markdown.find("| first | out | second | in | complex<float32> |  |") != std::string::npos) << markdown;
        expect(markdown.find("| third | out | second | in |  |  |") != std::string::npos) << markdown;
    };
#endif

    "a document made without a resolver keeps the type column empty"_test = [] {
        const std::string markdown = graphdoc::render(readFixture(), Format::Markdown, "Nested graph fixture");
        expect(markdown.find("| From | Port | To | Port | Type | Minimum buffer |") != std::string::npos) << "the column is there";
        expect(markdown.find("| front_end | 0 | sink | 0 |  | 4096 |") != std::string::npos) << markdown;
    };

    "a block's uninterpreted keys are documented with the block"_test = [] {
        const graphdoc::Level level = readFixture();
        expect(eq(level.blocks[2].uninterpretedKeys.size(), 1UZ)) << "layout_hint is not a key the reader interprets";

        for (const Format format : {Format::Markdown, Format::Html}) {
            const std::string document = graphdoc::render(level, format, "Nested graph fixture");
            expect(document.find("Other block keys") != std::string::npos) << "a block's unknown keys have no table";
            expect(document.find("layout_hint") != std::string::npos) << "the key itself is missing";
            expect(document.find("bottom-right") != std::string::npos) << "the value of the key is missing";
        }
    };

    "the diagram labels nodes and edges"_test = [] {
        const std::string diagram = graphdoc::diagramOf(readFixture(), "g");
        expect(diagram.starts_with("flowchart LR\n"));
        expect(diagram.find("[\"source\"]") != std::string::npos) << "a node carries the block name";
        expect(diagram.find("[[\"front_end\"]]") != std::string::npos) << "a subgraph node uses the subroutine shape";
        expect(eq(occurrences(diagram, " --> "), 2UZ)) << "one edge per connection of the level";
        expect(diagram.find("-- \"0 to 0\" -->") != std::string::npos) << "an edge is labeled with its ports";
    };

    "a node names the item type a key spells simply, and nothing where it does not"_test = [] {
        const auto level = graphdoc::read("blocks:\n"
                                          "  - id: qa::PpmFramer<complex<float32>>\n    parameters:\n      name: framer\n"
                                          "  - id: qa::RampSource\n    parameters:\n      name: plain\n"
                                          "  - id: qa::Convert<float32, uint8>\n    parameters:\n      name: pair\n"
                                          "  - id: qa::TagSink<DataSet<uint8>>\n    parameters:\n      name: nested\n");
        expect(fatal(level.has_value()));

        const std::string diagram = graphdoc::diagramOf(*level, "g");
        expect(diagram.find("[\"framer<br/>complex#lt;float32#gt;\"]") != std::string::npos) << diagram;
        expect(diagram.find("[\"plain\"]") != std::string::npos) << "a key with no template argument carries its name alone";
        expect(diagram.find("[\"pair\"]") != std::string::npos) << "and so does one with two arguments";
        expect(diagram.find("[\"nested\"]") != std::string::npos) << "and one whose argument is not a simple item type";

        const std::string drawn = graphdoc::svgOf(*level, "g");
        expect(eq(occurrences(drawn, "<text class=\"type\""), 1UZ)) << "the drawing labels the one node that has an item type";
        expect(drawn.find(">complex&lt;float32&gt;<") != std::string::npos) << drawn;
    };

    "a mermaid label spells its angle brackets as entities"_test = [] {
        const auto level = graphdoc::read("blocks:\n"
                                          "  - id: qa::PpmFramer<complex<float32>>\n    parameters:\n      name: radio\n"
                                          "  - id: qa::Scale\n    parameters:\n      name: gain\n"
                                          "connections:\n"
                                          "  - [radio, out, gain, in]\n");
        expect(fatal(level.has_value()));

        const std::string diagram = graphdoc::diagramOf(*level, "g");
        expect(diagram.find("complex#lt;float32#gt;") != std::string::npos) << diagram;
        // a renderer reads what stands between the quotes as HTML, so the line break is the only markup a label may hold
        for (std::size_t at = diagram.find('<'); at != std::string::npos; at = diagram.find('<', at + 1UZ)) {
            expect(diagram.compare(at, 5UZ, "<br/>") == 0) << std::format("a raw angle bracket at {} in {}", at, diagram);
        }
        expect(diagram.find("<float32>") == std::string::npos) << "so no label reaches a renderer as a tag";

        const std::string markdown = graphdoc::render(*level, Format::Markdown, "t");
        expect(markdown.find("complex#lt;float32#gt;") != std::string::npos) << "the fence of the document carries the same label";
    };

    "the block table sits in a box that scrolls, and the tables beside it do not"_test = [] {
        const std::string html = graphdoc::render(readFixture(), Format::Html, "Nested graph fixture");
        expect(eq(occurrences(html, "<div class=\"table bounded\">"), 3UZ)) << "the block table of each of the three levels";
        expect(html.find("max-height: 60vh") != std::string::npos) << "the box is bounded in height";
        expect(html.find("position: sticky") != std::string::npos) << "and the header row stays in view while it scrolls";
        expect(occurrences(html, "<div class=\"table\">") > 0UZ) << "a table that is not the block table keeps the plain box";

        const std::string markdown = graphdoc::render(readFixture(), Format::Markdown, "Nested graph fixture");
        expect(markdown.find("bounded") == std::string::npos) << "Markdown has one table shape";
    };

    "the HTML page carries no external reference"_test = [] {
        const std::string document = graphdoc::render(readFixture(), Format::Html, "Nested graph fixture");
        for (const std::string_view forbidden : {"<script", "<link", "<img", "http://", "https://", "@import"}) {
            expect(document.find(forbidden) == std::string::npos) << std::format("the page reaches outside itself: '{}'", forbidden);
        }
        expect(document.starts_with("<!DOCTYPE html>"));
        expect(document.find("<svg class=\"flowgraph\"") != std::string::npos) << "the diagram is drawn into the page";

        const auto markup = graphdoc::read("blocks:\n  - id: qa::Convert<float32>\n    parameters:\n      name: a & b\nconnections:\n  - [a & b, out, a & b, in]\n");
        expect(fatal(markup.has_value()));
        const std::string page = graphdoc::render(*markup, Format::Html, "t");
        expect(page.find("&lt;float32&gt;") != std::string::npos) << "the page escapes the angle brackets of its content";
        expect(page.find("a &amp; b") != std::string::npos) << "and its ampersands, in the tables and in the drawing alike";
    };

    "the HTML draws one picture per level, one node per block and one edge per connection"_test = [] {
        const std::string document = graphdoc::render(readFixture(), Format::Html, "Nested graph fixture");
        expect(eq(occurrences(document, "<svg class=\"flowgraph\""), 3UZ)) << "one drawing per graph level";
        expect(eq(occurrences(document, "<rect class=\"node"), 7UZ)) << "one node per block of every level";
        expect(eq(occurrences(document, "<path class=\"edge\""), 4UZ)) << "one edge per connection of every level";
        expect(eq(occurrences(document, "<pre class=\"mermaid\""), 0UZ)) << "the page carries no diagram as text";
        expect(document.find("viewBox=\"0 0 ") != std::string::npos) << "the drawing scales with the page";
        expect(eq(occurrences(document, "<div class=\"diagram\">"), 3UZ)) << "each drawing sits in a box of its own";
        expect(document.find("style=\"max-width:100%;height:auto\"") != std::string::npos) << "a drawing that fits the page is scaled to it";
        expect(document.find("min-width:") == std::string::npos) << "and is given no floor";
        expect(document.find("<rect class=\"inner\"") != std::string::npos) << "the subgraph node carries a second border";
        expect(document.find(">front_end<") != std::string::npos) << "a node is labeled with the block's name";
    };

    "a chain too long to scale into the page keeps its own size"_test = [] {
        constexpr std::size_t kRanks = 20UZ;
        std::string           chain  = "blocks:\n";
        for (std::size_t i = 0UZ; i < kRanks; ++i) {
            chain += std::format("  - id: qa::Scale\n    parameters:\n      name: b{}\n", i);
        }
        chain += "connections:\n";
        for (std::size_t i = 0UZ; i + 1UZ < kRanks; ++i) {
            chain += std::format("  - [b{}, out, b{}, in]\n", i, i + 1UZ);
        }

        const auto level = graphdoc::read(chain);
        expect(fatal(level.has_value()));
        const std::string drawn = graphdoc::svgOf(*level, "g");
        expect(drawn.find("style=\"max-width:100%;height:auto;min-width:") != std::string::npos) << "the drawing keeps its width and the box scrolls";
        expect(eq(occurrences(drawn, "<rect class=\"node\""), kRanks)) << "one node per block, whatever the width";
    };

    "the diagram stands before the blocks of its level in both formats"_test = [] {
        const std::string markdown = graphdoc::render(readFixture(), Format::Markdown, "Nested graph fixture");
        expect(markdown.find("## Diagram") < markdown.find("## Blocks")) << "the Markdown diagram follows the summary and leads the tables";
        expect(markdown.find("## Summary") < markdown.find("## Diagram"));
        expect(markdown.find("### Diagram") < markdown.find("### Blocks")) << "and so does a subgraph's";

        const std::string html = graphdoc::render(readFixture(), Format::Html, "Nested graph fixture");
        expect(html.find("<svg class=\"flowgraph\"") < html.find("<h2>Blocks</h2>"));
        expect(html.find("<h2>Summary</h2>") < html.find("<svg class=\"flowgraph\""));
    };

    "the same input yields the same bytes"_test = [] {
        const graphdoc::Level level = readFixture();
        expect(eq(graphdoc::render(level, Format::Markdown, "t"), graphdoc::render(level, Format::Markdown, "t")));
        expect(eq(graphdoc::render(level, Format::Html, "t"), graphdoc::render(level, Format::Html, "t")));
    };

    "a document with no blocks is empty and well formed"_test = [] {
        const auto level = graphdoc::read("something_else: 1\n");
        expect(level.has_value()) << "a graph with no blocks key is not an error";
        const std::string document = graphdoc::render(*level, Format::Markdown, "empty");
        expect(document.find("This graph level holds no blocks.") != std::string::npos);
        expect(document.find("This graph level holds no connections.") != std::string::npos);
        expect(eq(occurrences(document, "```mermaid"), 1UZ));
    };

    "a file that is not the dialect is reported"_test = [] {
        const auto level = graphdoc::read("value: !!int32 not-a-number\n");
        expect(!level.has_value()) << "a value that fails its own type tag must be reported";
        expect(!level.error().empty());
    };

    "a connection to a block the level does not hold is still drawn"_test = [] {
        const auto level = graphdoc::read("blocks:\n  - id: qa::Scale\n    parameters:\n      name: only\nconnections:\n  - [only, 0, missing, 0]\n");
        expect(level.has_value());
        const std::string diagram = graphdoc::diagramOf(*level, "g");
        expect(diagram.find("(unresolved)") != std::string::npos) << "the missing end is drawn and marked";
        expect(diagram.find("classDef unresolved") != std::string::npos);

        const std::string drawing = graphdoc::svgOf(*level, "g");
        expect(eq(occurrences(drawing, "<rect class=\"node"), 2UZ)) << "the missing end is a node of the drawing too";
        expect(drawing.find("<rect class=\"node unresolved\"") != std::string::npos) << "and it is drawn dashed";
        expect(drawing.find(">(unresolved)<") != std::string::npos);
    };

    "the drawing ranks a chain left to right and breaks a cycle at the edge that closes it"_test = [] {
        const auto chain = graphdoc::read("blocks:\n  - id: qa::Scale\n    parameters:\n      name: first\n  - id: qa::Scale\n    parameters:\n      name: second\nconnections:\n  - [first, out, second, in]\n");
        expect(chain.has_value());
        const std::string drawn      = graphdoc::svgOf(*chain, "g");
        const std::size_t firstNode  = drawn.find("<rect class=\"node\"");
        const std::size_t secondNode = drawn.find("<rect class=\"node\"", firstNode + 1UZ);
        expect(firstNode != std::string::npos && secondNode != std::string::npos);
        expect(leftEdgeOf(drawn, firstNode) < leftEdgeOf(drawn, secondNode)) << "the destination stands to the right of its source";

        const auto loop = graphdoc::read("blocks:\n  - id: qa::Scale\n    parameters:\n      name: first\n  - id: qa::Scale\n    parameters:\n      name: second\nconnections:\n  - [first, out, second, in]\n  - [second, out, first, in]\n");
        expect(loop.has_value());
        const std::string cycle      = graphdoc::svgOf(*loop, "g");
        const std::size_t cycleFirst = cycle.find("<rect class=\"node\"");
        const std::size_t cycleAfter = cycle.find("<rect class=\"node\"", cycleFirst + 1UZ);
        expect(leftEdgeOf(cycle, cycleFirst) < leftEdgeOf(cycle, cycleAfter)) << "the edge that closes the cycle does not rank its destination";
        expect(eq(occurrences(cycle, "<path class=\"edge\""), 2UZ)) << "both connections are still drawn";
    };

    "a file outside the importer's dialect is refused here as well"_test = [] {
        for (const Refusal& refusal : kRefusals) {
            const Run refused = describe(refusal.fileName, refusal.yaml);
            expect(eq(refused.exitCode, 1)) << std::format("{} left the status at {}: {}", refusal.fileName, refused.exitCode, refused.output);
            expect(refused.output.contains(refusal.message)) << std::format("{}: {}", refusal.fileName, refused.output);
#ifdef GR_ENABLE_BLOCK_REGISTRY
            expect(!importerAccepts(refusal.yaml)) << std::format("{}: the importer builds a graph from a file the tool refuses", refusal.fileName);
#endif
        }

        // the same program describes a file within the dialect and exits 0, so the refusals above belong to their files
        const Run described = describe("every_key.yaml", kEveryKey);
        expect(eq(described.exitCode, 0)) << described.output;
        expect(described.output.contains("# every_key.yaml")) << described.output;
    };

    "exported_ports is read inside a subgraph and left uninterpreted at the top level"_test = [] {
        const Run root = describe("root_exported_ports.yaml", kRootExportedPorts);
        expect(eq(root.exitCode, 0)) << "the importer reads no exported_ports at a document's top level" << root.output;

        const auto level = graphdoc::read(kRootExportedPorts);
        expect(fatal(level.has_value()));
        expect(level->exportedPorts.empty()) << "a top level exports no port";
        const std::string markdown = graphdoc::render(*level, Format::Markdown, "t");
        expect(markdown.find("| exported_ports |") != std::string::npos) << "the key belongs with the others the loader leaves alone" << markdown;
        expect(markdown.find("Exported ports") == std::string::npos) << "and no exported-ports table stands at the top level" << markdown;

        const Run nested = describe("subgraph_exported_ports.yaml", kSubgraphExportedPorts);
        expect(eq(nested.exitCode, 1)) << nested.output;
        expect(nested.output.contains("Unable to parse exported port (not a list)")) << nested.output;

#ifdef GR_ENABLE_BLOCK_REGISTRY
        // the two verdicts are the case: the same malformed entry passes at the top level and fails under a subgraph
        expect(importerAccepts(kRootExportedPorts)) << "the importer builds a graph from the top-level file";
        expect(!importerAccepts(kSubgraphExportedPorts)) << "and refuses the same entry inside a subgraph";
#endif
    };

    "the summary counts the blocks of a level by kind"_test = [] {
        const auto level = graphdoc::read(kEveryKey);
        expect(fatal(level.has_value()));
        const std::string markdown = graphdoc::render(*level, Format::Markdown, "t");
        expect(markdown.find("**Blocks at the top level**: 2 (1 NormalBlock, 1 ScheduledBlockGroup)") != std::string::npos) << markdown;
        expect(markdown.find("**Blocks**: 1 NormalBlock") != std::string::npos) << "a level of one kind names it without counting twice" << markdown;

        const std::string fixture = graphdoc::render(readFixture(), Format::Markdown, "Nested graph fixture");
        expect(fixture.find("**Blocks at the top level**: 3 (2 NormalBlock, 1 SUBGRAPH)") != std::string::npos) << "a block with no category is counted under the kind it has" << fixture;
    };

    "a scheduler's parameters, a nested metadata table, and every key left over have their own tables"_test = [] {
        const auto level = graphdoc::read(kEveryKey);
        expect(fatal(level.has_value()));
        const std::string markdown = graphdoc::render(*level, Format::Markdown, "t");
        expect(markdown.find("### Scheduler parameters") != std::string::npos) << markdown;
        expect(markdown.find("| thread_pool | \"workers\" |") != std::string::npos) << "the parameters of the scheduler that manages a subgraph" << markdown;
        expect(markdown.find("| inner_note | \"nested_metadata\" |") != std::string::npos) << "the definition metadata of a nested level" << markdown;
        expect(markdown.find("| scheduler.affinity |") != std::string::npos) << "a key of the scheduler map nothing renders" << markdown;
        expect(markdown.find("| ctx_parameters[0].note |") != std::string::npos) << "a key of a context entry nothing renders" << markdown;
        expect(markdown.find("| connections[0][5] |") != std::string::npos) << "a connection element past the buffer size" << markdown;
    };

    "every scalar of a file with every known key reaches both documents"_test = [] {
        const auto level = graphdoc::read(kEveryKey);
        expect(fatal(level.has_value()));
        for (const Format format : {Format::Markdown, Format::Html}) {
            const std::string document = graphdoc::render(*level, format, "t");
            for (const std::string_view scalar : kEveryKeyScalars) {
                expect(document.find(scalar) != std::string::npos) << std::format("'{}' is missing from the document", scalar);
            }
        }
    };

    "a block entry the importer passes over is listed rather than dropped"_test = [] {
        const auto level = graphdoc::read("blocks:\n  - 42\n  - id: qa::Scale\n    parameters:\n      name: gain\n");
        expect(fatal(level.has_value())) << "the importer passes over an entry that is not a map, so the document is written";
        expect(eq(level->blocks.size(), 1UZ));
        const std::string markdown = graphdoc::render(*level, Format::Markdown, "t");
        expect(markdown.find("| blocks[0] | 42 |") != std::string::npos) << markdown;
    };
};

int main() { /* not needed for UT */ }
