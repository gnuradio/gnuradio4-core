#ifndef GNURADIO_TOOLS_GRAPHDOC_HPP
#define GNURADIO_TOOLS_GRAPHDOC_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Tag.hpp>
#include <gnuradio-4.0/Value.hpp>
#include <gnuradio-4.0/YamlPmt.hpp>
#include <gnuradio-4.0/formatter/ValueFormatter.hpp>

#include "DocWriter.hpp"

namespace gr::tools::graphdoc {

/// The block-entry keys the reader interprets; anything else a file carries is listed as an
/// uninterpreted key rather than dropped, so a document never hides part of its input.
inline constexpr std::array<std::string_view, 9> kKnownBlockKeys{"id", "name", "unique_name", "block_category", "meta_information", "parameters", "ctx_parameters", "scheduler", "graph"};

/// the same for a graph level
inline constexpr std::array<std::string_view, 4> kKnownGraphKeys{"blocks", "connections", "exported_ports", "definition_metadata"};

[[nodiscard]] inline const property_map* mapOf(const pmt::Value& value) noexcept { return value.get_if<property_map>(); }

[[nodiscard]] inline const Tensor<pmt::Value>* listOf(const pmt::Value& value) noexcept { return value.get_if<Tensor<pmt::Value>>(); }

/// the string a value holds, or nothing: a non-string value yields a view with no data
[[nodiscard]] inline std::optional<std::string> stringOf(const pmt::Value& value) {
    const std::string_view view = value.value_or(std::string_view{});
    if (view.data() == nullptr) {
        return std::nullopt;
    }
    return std::string(view);
}

[[nodiscard]] inline const pmt::Value* entryOf(const property_map& map, std::string_view key) noexcept {
    const auto it = map.find(key);
    return it == map.cend() ? nullptr : &it->second;
}

/**
 * @brief One line of text for a value, with map keys in sorted order.
 *
 * `gr::pmt::to_string` walks a map in the hash order the container happens to have, which
 * differs between runs; a document that is compared against a pinned expectation needs the
 * same bytes every time, so maps are spelled here and everything else is delegated.
 */
[[nodiscard]] inline std::string valueText(const pmt::Value& value) {
    if (const property_map* map = mapOf(value); map != nullptr) {
        std::vector<std::string> entries;
        entries.reserve(map->size());
        for (const auto& [key, entry] : *map) {
            entries.emplace_back(std::format("{}: {}", std::string_view(key), valueText(entry)));
        }
        std::ranges::sort(entries);
        std::string out = "{";
        for (std::size_t i = 0UZ; i < entries.size(); ++i) {
            if (i != 0UZ) {
                out += ", ";
            }
            out += entries[i];
        }
        out += "}";
        return out;
    }
    return gr::pmt::to_string(value);
}

/// the sorted `key: value` lines of a map
[[nodiscard]] inline std::string mapLines(const property_map& map) {
    std::vector<std::string> lines;
    lines.reserve(map.size());
    for (const auto& [key, value] : map) {
        lines.emplace_back(std::format("{}: {}", std::string_view(key), valueText(value)));
    }
    std::ranges::sort(lines);
    std::string out;
    for (std::size_t i = 0UZ; i < lines.size(); ++i) {
        if (i != 0UZ) {
            out += "\n";
        }
        out += lines[i];
    }
    return out;
}

/// how a connection spells one of its two ends: an index, a name, or an index and a sub-index
[[nodiscard]] inline std::string portText(const pmt::Value& value) {
    if (const Tensor<pmt::Value>* pair = listOf(value); pair != nullptr) {
        std::string out;
        for (std::size_t i = 0UZ; i < pair->size(); ++i) {
            if (i != 0UZ) {
                out += ".";
            }
            out += valueText((*pair)[i]);
        }
        return out;
    }
    if (const std::optional<std::string> name = stringOf(value); name.has_value()) {
        return *name;
    }
    return valueText(value);
}

struct NamedText {
    std::string name;
    std::string value;
};

struct Connection {
    std::string sourceBlock;
    std::string sourcePort;
    std::string destinationBlock;
    std::string destinationPort;
    std::string minBufferSize; ///< empty unless the connection carries a fifth element
    std::string itemType;      ///< the type the items carry; empty where nothing answered for it
};

struct ExportedPort {
    std::string block;
    std::string direction;
    std::string internalName;
    std::string exportedName;
};

struct Block;

/// one graph level: what a file's top level and a subgraph's `graph` map both look like
struct Level {
    std::vector<Block>        blocks;
    std::vector<Connection>   connections;
    std::vector<ExportedPort> exportedPorts;
    std::vector<NamedText>    metadata;          ///< `definition_metadata`, sorted
    std::vector<NamedText>    uninterpretedKeys; ///< keys outside kKnownGraphKeys
};

struct Block {
    std::string            type; ///< the `id` field; "SUBGRAPH" for a nested graph
    std::string            name;
    std::string            uniqueName;
    std::string            schedulerId; ///< empty unless the subgraph is scheduler-managed
    std::string            parameters;  ///< sorted `key: value` lines
    std::string            metaInformation;
    std::vector<NamedText> contexts;          ///< one entry per `ctx_parameters` context
    std::vector<NamedText> uninterpretedKeys; ///< keys outside kKnownBlockKeys

    std::shared_ptr<Level> interior; ///< the nested graph of a SUBGRAPH entry

    [[nodiscard]] bool isSubgraph() const noexcept { return interior != nullptr; }
};

[[nodiscard]] inline std::vector<NamedText> uninterpretedKeysOf(const property_map& map, std::span<const std::string_view> known) {
    std::vector<NamedText> result;
    for (const auto& [key, value] : map) {
        const std::string_view view(key);
        if (std::ranges::find(known, view) == known.end()) {
            result.emplace_back(std::string(view), valueText(value));
        }
    }
    std::ranges::sort(result, [](const NamedText& a, const NamedText& b) { return a.name < b.name; });
    return result;
}

[[nodiscard]] inline std::vector<NamedText> sortedEntriesOf(const property_map& map) {
    std::vector<NamedText> result;
    result.reserve(map.size());
    for (const auto& [key, value] : map) {
        result.emplace_back(std::string(std::string_view(key)), valueText(value));
    }
    std::ranges::sort(result, [](const NamedText& a, const NamedText& b) { return a.name < b.name; });
    return result;
}

[[nodiscard]] Level readLevel(const property_map& map);

[[nodiscard]] inline Block readBlock(const property_map& entry) {
    Block block;
    if (const pmt::Value* id = entryOf(entry, "id"); id != nullptr) {
        block.type = stringOf(*id).value_or(valueText(*id));
    }
    if (const pmt::Value* uniqueName = entryOf(entry, "unique_name"); uniqueName != nullptr) {
        block.uniqueName = stringOf(*uniqueName).value_or(std::string{});
    }

    const property_map* parameters = nullptr;
    if (const pmt::Value* value = entryOf(entry, "parameters"); value != nullptr) {
        parameters = mapOf(*value);
    }
    if (parameters != nullptr) {
        block.parameters = mapLines(*parameters);
        if (const pmt::Value* name = entryOf(*parameters, "name"); name != nullptr) {
            block.name = stringOf(*name).value_or(std::string{});
        }
    }
    // a subgraph written before the parameters key carries its name at the top level
    if (block.name.empty()) {
        if (const pmt::Value* name = entryOf(entry, "name"); name != nullptr) {
            block.name = stringOf(*name).value_or(std::string{});
        }
    }

    if (const pmt::Value* meta = entryOf(entry, "meta_information"); meta != nullptr) {
        if (const property_map* map = mapOf(*meta); map != nullptr) {
            block.metaInformation = mapLines(*map);
        }
    }

    if (const pmt::Value* contexts = entryOf(entry, "ctx_parameters"); contexts != nullptr) {
        if (const Tensor<pmt::Value>* list = listOf(*contexts); list != nullptr) {
            for (const pmt::Value& contextValue : *list) {
                const property_map* context = mapOf(contextValue);
                if (context == nullptr) {
                    continue;
                }
                std::string label;
                if (const pmt::Value* name = entryOf(*context, gr::tag::CONTEXT.shortKey()); name != nullptr) {
                    label = stringOf(*name).value_or(valueText(*name));
                }
                if (const pmt::Value* time = entryOf(*context, gr::tag::CONTEXT_TIME.shortKey()); time != nullptr) {
                    label += std::format(" @ {}", valueText(*time));
                }
                std::string values;
                if (const pmt::Value* parameterValue = entryOf(*context, "parameters"); parameterValue != nullptr) {
                    if (const property_map* map = mapOf(*parameterValue); map != nullptr) {
                        values = mapLines(*map);
                    }
                }
                block.contexts.emplace_back(std::move(label), std::move(values));
            }
        }
    }

    if (const pmt::Value* scheduler = entryOf(entry, "scheduler"); scheduler != nullptr) {
        if (const property_map* map = mapOf(*scheduler); map != nullptr) {
            if (const pmt::Value* id = entryOf(*map, "id"); id != nullptr) {
                block.schedulerId = stringOf(*id).value_or(valueText(*id));
            }
        }
    }

    if (const pmt::Value* graph = entryOf(entry, "graph"); graph != nullptr) {
        if (const property_map* map = mapOf(*graph); map != nullptr) {
            block.interior = std::make_shared<Level>(readLevel(*map));
        }
    }

    block.uninterpretedKeys = uninterpretedKeysOf(entry, kKnownBlockKeys);
    return block;
}

inline Level readLevel(const property_map& map) {
    Level level;

    if (const pmt::Value* blocks = entryOf(map, "blocks"); blocks != nullptr) {
        if (const Tensor<pmt::Value>* list = listOf(*blocks); list != nullptr) {
            for (const pmt::Value& entry : *list) {
                if (const property_map* block = mapOf(entry); block != nullptr) {
                    level.blocks.push_back(readBlock(*block));
                }
            }
        }
    }

    if (const pmt::Value* connections = entryOf(map, "connections"); connections != nullptr) {
        if (const Tensor<pmt::Value>* list = listOf(*connections); list != nullptr) {
            for (const pmt::Value& entry : *list) {
                const Tensor<pmt::Value>* fields = listOf(entry);
                if (fields == nullptr || fields->size() < 4UZ) {
                    continue;
                }
                Connection connection{
                    .sourceBlock      = portText((*fields)[0]),
                    .sourcePort       = portText((*fields)[1]),
                    .destinationBlock = portText((*fields)[2]),
                    .destinationPort  = portText((*fields)[3]),
                    .minBufferSize    = fields->size() > 4UZ ? valueText((*fields)[4]) : std::string{},
                    .itemType         = {},
                };
                level.connections.push_back(std::move(connection));
            }
        }
    }

    if (const pmt::Value* exported = entryOf(map, "exported_ports"); exported != nullptr) {
        if (const Tensor<pmt::Value>* list = listOf(*exported); list != nullptr) {
            for (const pmt::Value& entry : *list) {
                const Tensor<pmt::Value>* fields = listOf(entry);
                if (fields == nullptr || fields->size() < 4UZ) {
                    continue;
                }
                level.exportedPorts.emplace_back(portText((*fields)[0]), portText((*fields)[1]), portText((*fields)[2]), portText((*fields)[3]));
            }
        }
    }

    if (const pmt::Value* metadata = entryOf(map, "definition_metadata"); metadata != nullptr) {
        if (const property_map* entries = mapOf(*metadata); entries != nullptr) {
            level.metadata = sortedEntriesOf(*entries);
        }
    }

    level.uninterpretedKeys = uninterpretedKeysOf(map, kKnownGraphKeys);
    return level;
}

/// Answers the type of the items an output port carries, given the block's registry key and the
/// port as a connection spells it. An empty answer leaves the connection's type cell blank.
using ConnectionTypeResolver = std::function<std::string(std::string_view blockType, std::string_view port)>;

/**
 * @brief Fills the item type of every connection of `level` and of every level below it.
 *
 * The type belongs to the source block's output port, so the lookup takes the source end: the
 * block the level holds under that name, and its `id` as the registry key. A subgraph is passed
 * over because SUBGRAPH is no key; its exported port is answered by the block behind it, which
 * this level does not name.
 */
inline void resolveConnectionTypes(Level& level, const ConnectionTypeResolver& typeOf) {
    if (!typeOf) {
        return;
    }
    std::map<std::string, const Block*, std::less<>> blockForName;
    for (const Block& block : level.blocks) {
        if (!block.uniqueName.empty()) {
            blockForName.emplace(block.uniqueName, &block);
        }
        if (!block.name.empty()) {
            blockForName.emplace(block.name, &block);
        }
    }
    for (Connection& connection : level.connections) {
        const auto source = blockForName.find(connection.sourceBlock);
        if (source != blockForName.end() && !source->second->isSubgraph()) {
            connection.itemType = typeOf(source->second->type, connection.sourcePort);
        }
    }
    for (Block& block : level.blocks) {
        if (block.isSubgraph()) {
            resolveConnectionTypes(*block.interior, typeOf);
        }
    }
}

/// how many subgraphs a level holds, counting every depth below it
[[nodiscard]] inline std::size_t countSubgraphs(const Level& level) {
    std::size_t count = 0UZ;
    for (const Block& block : level.blocks) {
        if (block.isSubgraph()) {
            ++count;
            count += countSubgraphs(*block.interior);
        }
    }
    return count;
}

inline void collectSchedulers(const Level& level, std::vector<std::string>& out) {
    for (const Block& block : level.blocks) {
        if (!block.schedulerId.empty()) {
            out.push_back(block.schedulerId);
        }
        if (block.isSubgraph()) {
            collectSchedulers(*block.interior, out);
        }
    }
}

/// the item types the framework spells with a short name, which is short enough for a node label
inline constexpr std::array<std::string_view, 12> kSimpleItemTypes{"int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64", "float32", "float64", "complex<float32>", "complex<float64>"};

/**
 * @brief The item type a node is labeled with, or nothing.
 *
 * A key whose one template argument is a scalar or a complex of one says what the block carries in
 * a few characters, which is all a node has room for: `PpmFramer<complex<float32>>` gives
 * `complex<float32>`. A key with no template argument, with more than one, or with one the
 * framework does not spell with a short name gives nothing, and the block table below the drawing
 * spells the type in full.
 */
[[nodiscard]] inline std::string nodeType(std::string_view type) {
    const std::size_t open = type.find('<');
    if (open == std::string_view::npos || !type.ends_with('>')) {
        return {};
    }
    const std::string_view argument = type.substr(open + 1UZ, type.size() - open - 2UZ);
    return std::ranges::find(kSimpleItemTypes, argument) == kSimpleItemTypes.end() ? std::string{} : std::string(argument);
}

/// A label for mermaid, which takes it between quotes and reads HTML inside them: the quote, the
/// entity marker and both angle brackets are spelled as entities, so a renderer shows
/// `complex<float32>` rather than reading `<float32>` as a tag. The line break the newline becomes
/// is the one piece of markup a label keeps.
[[nodiscard]] inline std::string mermaidLabel(std::string_view label) {
    std::string out;
    out.reserve(label.size());
    for (const char c : label) {
        switch (c) {
        case '"': out += "#quot;"; break;
        case '#': out += "#35;"; break;
        case '<': out += "#lt;"; break;
        case '>': out += "#gt;"; break;
        case '\n': out += "<br/>"; break;
        default: out += c; break;
        }
    }
    return out;
}

/**
 * @brief A `flowchart LR` of one graph level, the form a Markdown reader draws a diagram from.
 *
 * Node identifiers are minted from the level prefix and the block position, so they are stable
 * across runs and unique across nesting depths. An endpoint no block of the level answers to is
 * still drawn, as a node marked unresolved: a connection the document silently omitted would be
 * worse than one whose end is visibly missing.
 */
[[nodiscard]] inline std::string diagramOf(const Level& level, std::string_view prefix) {
    std::map<std::string, std::string, std::less<>> idForName;
    std::string                                     nodes;
    std::string                                     edges;

    for (std::size_t i = 0UZ; i < level.blocks.size(); ++i) {
        const Block&      block = level.blocks[i];
        const std::string id    = std::format("{}b{}", prefix, i);
        const std::string name  = block.name.empty() ? std::string("(unnamed)") : block.name;
        const std::string type  = nodeType(block.type);
        const std::string label = mermaidLabel(type.empty() ? name : std::format("{}\n{}", name, type));
        nodes += block.isSubgraph() ? std::format("    {}[[\"{}\"]]\n", id, label) : std::format("    {}[\"{}\"]\n", id, label);
        if (!block.uniqueName.empty()) {
            idForName.emplace(block.uniqueName, id);
        }
        if (!block.name.empty()) {
            idForName.emplace(block.name, id);
        }
    }

    std::size_t unresolved = 0UZ;
    auto        idOf       = [&](const std::string& blockName) {
        const auto it = idForName.find(blockName);
        if (it != idForName.end()) {
            return it->second;
        }
        const std::string id = std::format("{}x{}", prefix, unresolved++);
        nodes += std::format("    {}(\"{}\"):::unresolved\n", id, mermaidLabel(std::format("{}\n(unresolved)", blockName)));
        idForName.emplace(blockName, id);
        return id;
    };

    for (const Connection& connection : level.connections) {
        const std::string source      = idOf(connection.sourceBlock);
        const std::string destination = idOf(connection.destinationBlock);
        edges += std::format("    {} -- \"{} to {}\" --> {}\n", source, mermaidLabel(connection.sourcePort), mermaidLabel(connection.destinationPort), destination);
    }

    std::string diagram = "flowchart LR\n";
    diagram += nodes;
    diagram += edges;
    if (unresolved != 0UZ) {
        diagram += "    classDef unresolved stroke-dasharray: 4 3\n";
    }
    return diagram;
}

/// The geometry of a drawn flowgraph, in the units of the page. A label's width is estimated from a
/// fixed advance per character rather than measured: the two fonts are the page's own, which
/// `DocWriter`'s style sheet declares, and each constant is the advance that font takes for an
/// ASCII identifier.
inline constexpr double      kNameCharWidth   = 7.4; ///< 13 px of the page's sans stack
inline constexpr double      kTypeCharWidth   = 6.6; ///< 11 px of the page's monospace stack
inline constexpr double      kNodeHeight      = 42.0;
inline constexpr double      kNodeMinWidth    = 84.0;
inline constexpr double      kNodePadding     = 10.0;
inline constexpr double      kNodeGap         = 22.0; ///< between two nodes of one rank
inline constexpr double      kRankGap         = 74.0; ///< between two ranks, which the port labels share
inline constexpr double      kMargin          = 12.0;
inline constexpr double      kPageWidth       = 920.0; ///< the text column of the page, 60rem less its side padding
inline constexpr std::size_t kLabelCharacters = 28UZ;  ///< the widest label a node draws; a longer one is cut

/// the three characters that would otherwise end a text node or open a tag
[[nodiscard]] inline std::string svgText(std::string_view label) {
    std::string out;
    out.reserve(label.size());
    for (const char c : label) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out += c; break;
        }
    }
    return out;
}

/// a coordinate to one decimal, so the same graph draws the same bytes
[[nodiscard]] inline std::string coordinate(double value) { return std::format("{:.1f}", value); }

/// `text` cut to `limit` characters, its tail replaced by an ellipsis
[[nodiscard]] inline std::string fitLabel(std::string_view text, std::size_t limit) { return text.size() <= limit ? std::string(text) : std::format("{}...", text.substr(0UZ, limit - 3UZ)); }

/**
 * @brief One graph level drawn as an inline SVG: a layered flowgraph, the signal running left to right.
 *
 * A block's rank is the longest path to it from a source, so every edge but the one that closes a
 * cycle runs left to right; a depth-first walk in block order finds that edge and the ranking
 * leaves it out. Within a rank the blocks keep the order the file gives them. A subgraph carries a
 * second border inside its first, and an endpoint no block of the level answers to a dashed one:
 * a dashed node marks a connection that will not connect. Each drawing names its arrow marker
 * from the level prefix, so the identifiers of several drawings on one page do not collide.
 */
[[nodiscard]] inline std::string svgOf(const Level& level, std::string_view prefix) {
    struct Node {
        std::string name;
        std::string type;
        bool        subgraph   = false;
        bool        unresolved = false;
        double      x          = 0.0;
        double      y          = 0.0;
        double      width      = kNodeMinWidth;
    };
    struct Edge {
        std::size_t from = 0UZ;
        std::size_t to   = 0UZ;
        std::string fromPort;
        std::string toPort;
    };

    std::vector<Node>                               nodes;
    std::map<std::string, std::size_t, std::less<>> indexForName;
    for (const Block& block : level.blocks) {
        const std::size_t at = nodes.size();
        nodes.push_back({.name = block.name.empty() ? std::string("(unnamed)") : fitLabel(block.name, kLabelCharacters), .type = nodeType(block.type), .subgraph = block.isSubgraph()});
        if (!block.uniqueName.empty()) {
            indexForName.emplace(block.uniqueName, at);
        }
        if (!block.name.empty()) {
            indexForName.emplace(block.name, at);
        }
    }

    auto indexOf = [&nodes, &indexForName](const std::string& blockName) -> std::size_t {
        if (const auto held = indexForName.find(blockName); held != indexForName.end()) {
            return held->second;
        }
        const std::size_t at = nodes.size();
        nodes.push_back({.name = fitLabel(blockName, kLabelCharacters), .type = "(unresolved)", .unresolved = true});
        indexForName.emplace(blockName, at);
        return at;
    };

    std::vector<Edge> edges;
    edges.reserve(level.connections.size());
    for (const Connection& connection : level.connections) {
        const std::size_t from = indexOf(connection.sourceBlock);
        const std::size_t to   = indexOf(connection.destinationBlock);
        edges.push_back({.from = from, .to = to, .fromPort = connection.sourcePort, .toPort = connection.destinationPort});
    }

    const std::size_t                     nodeCount = nodes.size();
    std::vector<std::vector<std::size_t>> outgoing(nodeCount);
    for (std::size_t e = 0UZ; e < edges.size(); ++e) {
        outgoing[edges[e].from].push_back(e);
    }

    std::vector<bool>                                closesCycle(edges.size(), false);
    std::vector<std::uint8_t>                        state(nodeCount, std::uint8_t{0}); // 0 not reached, 1 on the walk, 2 left behind
    std::vector<std::pair<std::size_t, std::size_t>> walk;
    for (std::size_t start = 0UZ; start < nodeCount; ++start) {
        if (state[start] != std::uint8_t{0}) {
            continue;
        }
        state[start] = std::uint8_t{1};
        walk.emplace_back(start, 0UZ);
        while (!walk.empty()) {
            const std::size_t node = walk.back().first;
            if (walk.back().second == outgoing[node].size()) {
                state[node] = std::uint8_t{2};
                walk.pop_back();
                continue;
            }
            const std::size_t edge = outgoing[node][walk.back().second++];
            const std::size_t next = edges[edge].to;
            if (state[next] == std::uint8_t{1}) {
                closesCycle[edge] = true;
            } else if (state[next] == std::uint8_t{0}) {
                state[next] = std::uint8_t{1};
                walk.emplace_back(next, 0UZ);
            }
        }
    }

    std::vector<std::size_t> rank(nodeCount, 0UZ);
    for (std::size_t pass = 0UZ; pass < nodeCount; ++pass) {
        bool moved = false;
        for (std::size_t e = 0UZ; e < edges.size(); ++e) {
            if (!closesCycle[e] && rank[edges[e].from] + 1UZ > rank[edges[e].to]) {
                rank[edges[e].to] = rank[edges[e].from] + 1UZ;
                moved             = true;
            }
        }
        if (!moved) {
            break;
        }
    }

    std::size_t rankCount = 0UZ;
    for (const std::size_t r : rank) {
        rankCount = std::max(rankCount, r + 1UZ);
    }
    std::vector<std::vector<std::size_t>> column(rankCount);
    for (std::size_t i = 0UZ; i < nodeCount; ++i) {
        column[rank[i]].push_back(i);
    }

    auto columnHeight = [&column](std::size_t r) { return column[r].empty() ? 0.0 : static_cast<double>(column[r].size()) * kNodeHeight + static_cast<double>(column[r].size() - 1UZ) * kNodeGap; };

    std::vector<double> columnWidth(rankCount, kNodeMinWidth);
    double              tallest = 0.0;
    for (std::size_t r = 0UZ; r < rankCount; ++r) {
        for (const std::size_t i : column[r]) {
            const double text = std::max(static_cast<double>(nodes[i].name.size()) * kNameCharWidth, static_cast<double>(nodes[i].type.size()) * kTypeCharWidth);
            columnWidth[r]    = std::max(columnWidth[r], text + 2.0 * kNodePadding);
        }
        tallest = std::max(tallest, columnHeight(r));
    }

    double left = kMargin;
    for (std::size_t r = 0UZ; r < rankCount; ++r) {
        double top = kMargin + (tallest - columnHeight(r)) / 2.0;
        for (const std::size_t i : column[r]) {
            nodes[i].x     = left;
            nodes[i].y     = top;
            nodes[i].width = columnWidth[r];
            top += kNodeHeight + kNodeGap;
        }
        left += columnWidth[r] + kRankGap;
    }
    const double width  = rankCount == 0UZ ? 2.0 * kMargin : left - kRankGap + kMargin;
    const double height = tallest + 2.0 * kMargin;

    // the edges first, so that a node sits over the curves that reach it
    std::string body;
    for (const Edge& edge : edges) {
        const Node&  from  = nodes[edge.from];
        const Node&  to    = nodes[edge.to];
        const double x1    = from.x + from.width;
        const double y1    = from.y + kNodeHeight / 2.0;
        const double x2    = to.x;
        const double y2    = to.y + kNodeHeight / 2.0;
        const double reach = std::max(kRankGap / 2.0, (x2 - x1) * 0.45);
        body += std::format("<path class=\"edge\" marker-end=\"url(#{}-arrow)\" d=\"M {} {} C {} {}, {} {}, {} {}\"/>\n", prefix, coordinate(x1), coordinate(y1), coordinate(x1 + reach), coordinate(y1), coordinate(x2 - reach), coordinate(y2), coordinate(x2), coordinate(y2));
        if (!edge.fromPort.empty()) {
            body += std::format("<text class=\"port\" x=\"{}\" y=\"{}\">{}</text>\n", coordinate(x1 + 5.0), coordinate(y1 - 5.0), svgText(fitLabel(edge.fromPort, kLabelCharacters)));
        }
        if (!edge.toPort.empty()) {
            body += std::format("<text class=\"port\" x=\"{}\" y=\"{}\" text-anchor=\"end\">{}</text>\n", coordinate(x2 - 5.0), coordinate(y2 - 5.0), svgText(fitLabel(edge.toPort, kLabelCharacters)));
        }
    }
    for (const Node& node : nodes) {
        body += std::format("<rect class=\"node{}\" x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\" rx=\"7\" ry=\"7\"/>\n", node.unresolved ? " unresolved" : "", coordinate(node.x), coordinate(node.y), coordinate(node.width), coordinate(kNodeHeight));
        if (node.subgraph) {
            body += std::format("<rect class=\"inner\" x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\" rx=\"5\" ry=\"5\"/>\n", coordinate(node.x + 3.0), coordinate(node.y + 3.0), coordinate(node.width - 6.0), coordinate(kNodeHeight - 6.0));
        }
        // a node with nothing on its second line carries the name in the middle of the box instead
        body += std::format("<text class=\"name\" x=\"{}\" y=\"{}\" text-anchor=\"middle\">{}</text>\n", coordinate(node.x + node.width / 2.0), coordinate(node.y + (node.type.empty() ? 26.0 : 17.0)), svgText(node.name));
        if (!node.type.empty()) {
            body += std::format("<text class=\"type\" x=\"{}\" y=\"{}\" text-anchor=\"middle\">{}</text>\n", coordinate(node.x + node.width / 2.0), coordinate(node.y + 31.0), svgText(node.type));
        }
    }

    // A drawing scaled into the page's own width stays legible while it is roughly as wide as the
    // page. A long chain is not: twenty ranks across a text column put the labels below a pixel, so
    // a drawing wider than the column keeps its own size and the box around it scrolls instead.
    const std::string floor = width > kPageWidth ? std::format(";min-width:{}px", coordinate(width)) : std::string{};

    return std::format("<svg class=\"flowgraph\" viewBox=\"0 0 {0} {1}\" width=\"{0}\" height=\"{1}\" style=\"max-width:100%;height:auto{4}\">\n"
                       "<defs><marker id=\"{2}-arrow\" viewBox=\"0 0 8 6\" refX=\"8\" refY=\"3\" markerWidth=\"8\" markerHeight=\"6\" markerUnits=\"userSpaceOnUse\" orient=\"auto\"><path class=\"arrow\" d=\"M 0 0 L 8 3 L 0 6 z\"/></marker></defs>\n"
                       "{3}</svg>\n",
        coordinate(width), coordinate(height), prefix, body, floor);
}

/// The name cell of the block table: the name, and the unique name in parentheses under it where
/// the file gives one, so that a column standing empty in most documents is not spent on it.
[[nodiscard]] inline std::string nameCell(const Block& block) { return block.uniqueName.empty() ? block.name : std::format("{}\n({})", block.name, block.uniqueName); }

/// The type cell: the type name, a templated type's arguments on a second line, and a kind other
/// than a plain block in parentheses on a third, for the same reason the name cell folds.
[[nodiscard]] inline std::string typeCell(const Block& block) {
    std::string cell = block.type;
    if (const std::size_t open = block.type.find('<'); open != std::string::npos) {
        cell = std::format("{}\n{}", block.type.substr(0UZ, open), block.type.substr(open));
    }

    std::string kind;
    auto        add = [&kind](std::string_view text) { kind += kind.empty() ? std::string(text) : std::format(", {}", text); };
    if (block.isSubgraph()) {
        add("subgraph");
    }
    if (!block.schedulerId.empty()) {
        add(std::format("scheduler {}", block.schedulerId));
    }
    return kind.empty() ? cell : std::format("{}\n({})", cell, kind);
}

inline void writeNamedTable(DocWriter& writer, std::string_view firstColumn, std::string_view secondColumn, const std::vector<NamedText>& entries) {
    if (entries.empty()) {
        return;
    }
    std::vector<std::vector<std::string>> rows;
    rows.reserve(entries.size());
    for (const NamedText& entry : entries) {
        rows.push_back({entry.name, entry.value});
    }
    const std::array<std::string_view, 2> headers{firstColumn, secondColumn};
    writer.table(headers, rows);
}

inline void writeLevelBody(DocWriter& writer, const Level& level, std::size_t depth, std::string_view prefix) {
    const std::size_t headingLevel = depth + 2UZ;

    // the picture comes first; the tables below it give the detail a node label cuts short
    writer.heading(headingLevel, "Diagram");
    if (writer.format() == Format::Html) {
        writer.svg(svgOf(level, prefix));
    } else {
        writer.mermaid(diagramOf(level, prefix));
    }

    writer.heading(headingLevel, "Blocks");
    if (level.blocks.empty()) {
        writer.paragraph("This graph level holds no blocks.");
    } else {
        std::vector<std::vector<std::string>> rows;
        rows.reserve(level.blocks.size());
        for (const Block& block : level.blocks) {
            std::string parameters = block.parameters;
            for (const NamedText& context : block.contexts) {
                if (!parameters.empty()) {
                    parameters += "\n";
                }
                parameters += std::format("[context {}] {}", context.name, context.value);
            }
            rows.push_back({nameCell(block), typeCell(block), parameters, block.metaInformation});
        }
        // the block table is the one that grows without bound, so it is the one held in a box
        const std::array<std::string_view, 4> headers{"Name", "Type", "Parameters", "Meta information"};
        writer.table(headers, rows, true);
    }

    writer.heading(headingLevel, "Connections");
    if (level.connections.empty()) {
        writer.paragraph("This graph level holds no connections.");
    } else {
        std::vector<std::vector<std::string>> rows;
        rows.reserve(level.connections.size());
        for (const Connection& connection : level.connections) {
            rows.push_back({connection.sourceBlock, connection.sourcePort, connection.destinationBlock, connection.destinationPort, connection.itemType, connection.minBufferSize});
        }
        const std::array<std::string_view, 6> headers{"From", "Port", "To", "Port", "Type", "Minimum buffer"};
        writer.table(headers, rows);
    }

    if (!level.uninterpretedKeys.empty()) {
        writer.heading(headingLevel, "Other keys");
        writeNamedTable(writer, "Key", "Value", level.uninterpretedKeys);
    }

    // A block's uninterpreted keys belong to that block, so they are named with it. They travel in
    // one table per level, beside the level's own, rather than in a further column of the block
    // table that every graph without such a key would carry empty.
    std::vector<std::vector<std::string>> blockKeys;
    for (std::size_t i = 0UZ; i < level.blocks.size(); ++i) {
        const Block&      block = level.blocks[i];
        const std::string name  = block.name.empty() ? std::format("block {}", i) : block.name;
        for (const NamedText& key : block.uninterpretedKeys) {
            blockKeys.push_back({name, key.name, key.value});
        }
    }
    if (!blockKeys.empty()) {
        writer.heading(headingLevel, "Other block keys");
        const std::array<std::string_view, 3> headers{"Block", "Key", "Value"};
        writer.table(headers, blockKeys);
    }
}

inline void writeSubgraphs(DocWriter& writer, const Level& level, std::size_t depth, std::string_view prefix, std::string_view path) {
    for (std::size_t i = 0UZ; i < level.blocks.size(); ++i) {
        const Block& block = level.blocks[i];
        if (!block.isSubgraph()) {
            continue;
        }
        const std::string name       = block.name.empty() ? std::format("subgraph {}", i) : block.name;
        const std::string nestedPath = path.empty() ? name : std::format("{} / {}", path, name);
        const std::string nestedTag  = std::format("{}s{}", prefix, i);
        writer.heading(depth + 2UZ, std::format("Subgraph: {}", nestedPath), writer.anchor(std::format("subgraph {}", nestedPath)));

        std::vector<std::string> facts;
        facts.push_back(writer.labeled("Type", block.type));
        if (!block.uniqueName.empty()) {
            facts.push_back(writer.labeled("Unique name", block.uniqueName));
        }
        facts.push_back(writer.labeled("Scheduler", block.schedulerId.empty() ? "none, the parent schedules its blocks" : block.schedulerId));
        facts.push_back(writer.labeled("Blocks", std::to_string(block.interior->blocks.size())));
        facts.push_back(writer.labeled("Connections", std::to_string(block.interior->connections.size())));
        writer.rawBullets(facts);

        if (!block.interior->exportedPorts.empty()) {
            writer.heading(depth + 3UZ, "Exported ports");
            std::vector<std::vector<std::string>> rows;
            rows.reserve(block.interior->exportedPorts.size());
            for (const ExportedPort& port : block.interior->exportedPorts) {
                rows.push_back({port.exportedName, port.direction, port.block, port.internalName});
            }
            const std::array<std::string_view, 4> headers{"Exported as", "Direction", "Inner block", "Inner port"};
            writer.table(headers, rows);
        }

        writeLevelBody(writer, *block.interior, depth + 1UZ, nestedTag);
        writeSubgraphs(writer, *block.interior, depth + 1UZ, nestedTag, nestedPath);
    }
}

/// the whole document for a graph already read into a level
[[nodiscard]] inline std::string render(const Level& level, Format format, std::string title) {
    DocWriter writer(format, std::move(title));

    std::vector<std::string> schedulers;
    collectSchedulers(level, schedulers);
    std::ranges::sort(schedulers);
    schedulers.erase(std::ranges::unique(schedulers).begin(), schedulers.end());

    writer.heading(2UZ, "Summary");
    std::vector<std::string> summary;
    summary.push_back(writer.labeled("Blocks at the top level", std::to_string(level.blocks.size())));
    summary.push_back(writer.labeled("Connections at the top level", std::to_string(level.connections.size())));
    summary.push_back(writer.labeled("Subgraphs, all depths", std::to_string(countSubgraphs(level))));
    if (!schedulers.empty()) {
        std::string names;
        for (std::size_t i = 0UZ; i < schedulers.size(); ++i) {
            names += (i == 0UZ ? "" : ", ") + schedulers[i];
        }
        summary.push_back(writer.labeled("Schedulers", names));
    }
    writer.rawBullets(summary);

    if (!level.metadata.empty()) {
        writer.heading(2UZ, "Definition metadata");
        writeNamedTable(writer, "Key", "Value", level.metadata);
    }

    if (!level.exportedPorts.empty()) {
        writer.heading(2UZ, "Exported ports");
        std::vector<std::vector<std::string>> rows;
        rows.reserve(level.exportedPorts.size());
        for (const ExportedPort& port : level.exportedPorts) {
            rows.push_back({port.exportedName, port.direction, port.block, port.internalName});
        }
        const std::array<std::string_view, 4> headers{"Exported as", "Direction", "Inner block", "Inner port"};
        writer.table(headers, rows);
    }

    writeLevelBody(writer, level, 0UZ, "g");
    writeSubgraphs(writer, level, 0UZ, "g", {});

    return writer.finish();
}

/// reads the YAML with the framework's own parser, so the dialect is exactly the importer's
[[nodiscard]] inline std::expected<Level, std::string> read(std::string_view yaml) {
    const auto parsed = gr::pmt::yaml::deserialize(yaml);
    if (!parsed.has_value()) {
        return std::unexpected(std::format("line {}, column {}: {}", parsed.error().line, parsed.error().column, parsed.error().message));
    }
    return readLevel(*parsed);
}

} // namespace gr::tools::graphdoc

#endif // GNURADIO_TOOLS_GRAPHDOC_HPP
