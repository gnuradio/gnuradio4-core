#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "GraphDoc.hpp"

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

    "the connection table carries a type column, which is blank where nothing answered for it"_test = [] {
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
};

int main() { /* not needed for UT */ }
