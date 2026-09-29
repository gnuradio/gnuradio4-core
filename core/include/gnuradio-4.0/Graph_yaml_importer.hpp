#ifndef GNURADIO_GRAPH_YAML_IMPORTER_H
#define GNURADIO_GRAPH_YAML_IMPORTER_H

#include <algorithm>
#include <array>
#include <map>
#include <ranges>
#include <set>
#include <vector>

#include <gnuradio-4.0/meta/indirect.hpp>

#include <gnuradio-4.0/YamlPmt.hpp>

#include "BlockModel.hpp"
#include "Graph.hpp"
#include "PluginLoader.hpp"

namespace gr {

/**
 * Settings a caller puts in force over a graph file's own, each entry for one block at the file's top level.
 *
 * A key names the one block that carries it as its `unique_name` or its `name`. A key no block carries, a key two
 * blocks carry and a key for a subgraph are refused before any block is made. The entry's map replaces those keys of
 * the block's `parameters`. Every block is constructed with the merged parameters, and its settings load the same map.
 * Its constructor, `settingsChanged()` and `start()` see the given values. A port count such as `n_inputs` sizes the
 * ports before the connections are made.
 */
using BlockSettings = std::map<std::string, property_map, std::less<>>;

namespace detail {

template<typename T>
inline std::expected<T, gr::Error> getProperty(const gr::property_map& map, std::string_view propertyName) {
    auto it = map.find(propertyName);
    if (it == map.cend()) {
        return std::unexpected(gr::Error(std::format("Missing field {} in YAML object", propertyName)));
    }

    if constexpr (std::is_same_v<T, std::string>) {
        auto value = it->second.value_or(std::string_view{});
        if (value.data() != nullptr) {
            return std::string(value);
        }
    } else {
        // the non-terminating form, so that the incorrect-type report below is reachable for every T
        auto value = checked_access_ptr<const T, false>{it->second.get_if<T>()};
        if (value != nullptr) {
            return *value;
        }
    }

    return std::unexpected(gr::Error(std::format("Field {} in YAML object {} has an incorrect type {}:{} instead of {}", propertyName, map, it->second.value_type(), it->second.container_type(), gr::meta::type_name<T>())));
}

template<typename T>
inline std::expected<T, gr::Error> getProperty(const gr::property_map& map, std::string_view propertyName, const auto&... propertySubNames)
requires(sizeof...(propertySubNames) > 0)
{
    static_assert((std::is_convertible_v<decltype(propertySubNames), std::string_view> && ...));
    auto it = map.find(propertyName);
    if (it == map.cend()) {
        return std::unexpected(gr::Error(std::format("Missing field {} in YAML object", propertyName)));
    }

    auto value = checked_access_ptr<const gr::property_map, false>{it->second.get_if<gr::property_map>()};
    if (value == nullptr) {
        return std::unexpected(gr::Error(std::format("Field {} in YAML object has an incorrect type {}:{} instead of gr::property_map", propertyName, it->second.value_type(), it->second.container_type())));
    }

    return getProperty<T>(*value, propertySubNames...);
}

template<typename T>
T getOrThrow(std::expected<T, gr::Error>&& expectedValue, std::source_location location = std::source_location::current()) {
    if (!expectedValue) {
        throw gr::exception(std::format("Got an error {}, caller {}:{}", expectedValue.error().message, location.file_name(), location.line()));
    } else {
        return *expectedValue;
    }
}

/**
 * The blocks one graph level produced, indexed by the two names a YAML file may address them by.
 *
 * `unique_name` identifies a block within a file; `name` is a label and may repeat, so a repeated name
 * is recorded as ambiguous and reported instead of silently resolving to the last block that carried it.
 */
struct LoadedBlocks {
    std::map<std::string, std::shared_ptr<BlockModel>, std::less<>> byUniqueName;
    std::map<std::string, std::shared_ptr<BlockModel>, std::less<>> byName;
    std::set<std::string, std::less<>>                              ambiguousNames;

    void add(std::string_view uniqueName, std::string_view name, std::shared_ptr<BlockModel> block) {
        if (!uniqueName.empty()) {
            byUniqueName.insert_or_assign(std::string(uniqueName), block);
        }
        if (!name.empty() && !byName.try_emplace(std::string(name), std::move(block)).second) {
            ambiguousNames.emplace(name);
        }
    }

    [[nodiscard]] std::expected<std::shared_ptr<BlockModel>, gr::Error> find(std::string_view key) const {
        if (const auto it = byUniqueName.find(key); it != byUniqueName.cend()) {
            return it->second;
        }
        if (ambiguousNames.contains(key)) {
            return std::unexpected(gr::Error(std::format("'{}' is the name of more than one block, address it by its unique_name", key)));
        }
        if (const auto it = byName.find(key); it != byName.cend()) {
            return it->second;
        }
        return std::unexpected(gr::Error(std::format("Unknown block '{}'", key)));
    }
};

/// The number of single-character edits that turn one string into the other.
inline std::size_t editDistance(std::string_view from, std::string_view to) {
    std::vector<std::size_t> row(to.size() + 1UZ);
    for (std::size_t j = 0UZ; j < row.size(); ++j) {
        row[j] = j;
    }
    for (std::size_t i = 1UZ; i <= from.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0]               = i;
        for (std::size_t j = 1UZ; j <= to.size(); ++j) {
            const std::size_t above = row[j];
            row[j]                  = std::min({row[j] + 1UZ, row[j - 1UZ] + 1UZ, diagonal + (from[i - 1UZ] == to[j - 1UZ] ? 0UZ : 1UZ)});
            diagonal                = above;
        }
    }
    return row[to.size()];
}

/// Up to three of the candidates nearest to the name, nearest first, joined for a message.
inline std::string closestNames(std::string_view name, const std::ranges::input_range auto& candidates) {
    std::vector<std::pair<std::size_t, std::string_view>> ranked;
    for (const auto& candidate : candidates) {
        ranked.emplace_back(editDistance(name, candidate), candidate);
    }
    std::ranges::sort(ranked);
    std::string joined;
    for (const auto& [distance, candidate] : ranked | std::views::take(3)) {
        joined += std::format("{}'{}'", joined.empty() ? "" : ", ", candidate);
    }
    return joined;
}

/// What the loader reads of a block before it makes the block.
struct BlockIdentity {
    std::string type;
    std::string uniqueName;
    std::string name;

    [[nodiscard]] bool isSubgraph() const noexcept { return type == "SUBGRAPH"; }
};

inline BlockIdentity readBlockIdentity(const property_map& grcBlock) {
    // the type decides which fields are required, so it is read first
    BlockIdentity identity{.type = getOrThrow(getProperty<std::string>(grcBlock, "id"sv)), .uniqueName = getProperty<std::string>(grcBlock, "unique_name"sv).value_or(std::string{}), .name = {}};
    auto          fromParameters = getProperty<std::string>(grcBlock, "parameters"sv, "name"sv);
    if (fromParameters.has_value() || !identity.isSubgraph()) {
        identity.name = getOrThrow(std::move(fromParameters));
    } else {
        // subgraphs written before the parameters key carry their name at the top level
        identity.name = getProperty<std::string>(grcBlock, "name"sv).value_or(std::string{});
    }
    return identity;
}

/// The entry of `overrides` each block takes, indexed by the block's position in the file's list of blocks.
///
/// Each key is resolved once, over all the blocks, to the one block that carries it as its unique_name or its name. A
/// key no block carries, a key two blocks carry, a key for a subgraph and two keys for one block are refused.
inline std::vector<const property_map*> resolveBlockSettings(const Tensor<pmt::Value>& blocks, const BlockSettings& overrides) {
    std::vector<const property_map*> settingsByPosition(blocks.size(), nullptr);
    if (overrides.empty()) {
        return settingsByPosition;
    }

    std::vector<std::pair<std::size_t, BlockIdentity>> identities;
    std::set<std::string_view, std::less<>>            carriedKeys;
    for (std::size_t position = 0UZ; position < blocks.size(); ++position) {
        if (const auto grcBlock = checked_access_ptr<const property_map, false>{blocks[position].get_if<property_map>()}; grcBlock != nullptr) {
            identities.emplace_back(position, readBlockIdentity(*grcBlock));
        }
    }
    for (const auto& [position, identity] : identities) {
        carriedKeys.insert(identity.uniqueName);
        carriedKeys.insert(identity.name);
    }
    carriedKeys.erase(std::string_view{});

    auto describe = [](const std::pair<std::size_t, BlockIdentity>& block) {
        const auto& [position, identity] = block;
        return identity.uniqueName.empty() ? std::format("block {} '{}' of type '{}'", position, identity.name, identity.type) : std::format("block {} '{}' of type '{}' with unique_name '{}'", position, identity.name, identity.type, identity.uniqueName);
    };

    std::vector<std::string_view> keyByPosition(blocks.size());
    for (const auto& [key, settings] : overrides) {
        std::vector<const std::pair<std::size_t, BlockIdentity>*> carriers;
        for (const auto& block : identities) {
            const BlockIdentity& identity = block.second;
            if (!key.empty() && (identity.uniqueName == key || identity.name == key)) {
                carriers.push_back(std::addressof(block));
            }
        }

        if (carriers.empty()) {
            throw gr::exception(std::format("settings are given for block '{}', and the graph holds no block of that name; the nearest are {}", key, closestNames(key, carriedKeys)));
        }
        if (carriers.size() > 1UZ) {
            std::string named;
            for (const auto* block : carriers) {
                named += std::format("{}{}", named.empty() ? "" : ", ", describe(*block));
            }
            throw gr::exception(std::format("settings are given for block '{}', and {} blocks carry that unique_name or name: {}; a key names one block", key, carriers.size(), named));
        }
        const auto& [position, identity] = *carriers.front();
        if (identity.isSubgraph()) {
            throw gr::exception(std::format("settings are given for '{}', which is a subgraph and holds no settings of its own", key));
        }
        if (settingsByPosition[position] != nullptr) {
            throw gr::exception(std::format("settings are given twice for {}, as '{}' and as '{}'", describe(*carriers.front()), keyByPosition[position], key));
        }
        settingsByPosition[position] = std::addressof(settings);
        keyByPosition[position]      = key;
    }
    return settingsByPosition;
}

/// Throws for a key of `overrides` the block does not declare. The settings map would otherwise keep that key as meta
/// information and apply nothing.
inline void checkDeclared(const BlockModel& block, std::string_view blockName, const property_map& overrides) {
    const std::set<std::string>& declared = block.settings().writableMembers();
    for (const auto& [key, value] : overrides) {
        const std::string_view name(key.data(), key.size());
        if (!declared.contains(std::string(name))) {
            throw gr::exception(std::format("block '{}' of type '{}' declares no setting named '{}'; the nearest are {}", blockName, block.typeName(), name, closestNames(name, declared)));
        }
    }
}

/// The file's parameters of one block with the caller's settings, if any, in place of the same keys.
inline property_map mergedParameters(const property_map* fromFile, const property_map* overrides) {
    property_map merged = fromFile != nullptr ? *fromFile : property_map{};
    if (overrides != nullptr) {
        for (const auto& [key, value] : *overrides) {
            merged.insert_or_assign(key, value);
        }
    }
    return merged;
}

inline LoadedBlocks loadGraphFromMap(PluginLoader& loader, gr::Graph& resultGraph, gr::property_map yaml, const BlockSettings& overrides = {}, std::source_location location = std::source_location::current()) {
    LoadedBlocks createdBlocks;

    Tensor<pmt::Value> blks;
    if (auto it = yaml.find("blocks"); it != yaml.end()) {
        if (const auto blkRef = checked_access_ptr<Tensor<pmt::Value>, false>{it->second.get_if<Tensor<pmt::Value>>()}; blkRef != nullptr) {
            blks = *blkRef;
        }
    }

    const std::vector<const property_map*> settingsByPosition = resolveBlockSettings(blks, overrides);

    for (std::size_t position = 0UZ; position < blks.size(); ++position) {
        const auto _grcBlock = checked_access_ptr<const property_map, false>{blks[position].get_if<property_map>()};
        if (_grcBlock == nullptr) {
            continue;
        }
        const auto& grcBlock = *_grcBlock;

        const BlockIdentity identity        = readBlockIdentity(grcBlock);
        const std::string&  blockType       = identity.type;
        const bool          isSubgraph      = identity.isSubgraph();
        const std::string&  blockUniqueName = identity.uniqueName;
        const std::string&  blockName       = identity.name;

        // the block's own entries win: a block regenerates what it says about itself when it is
        // constructed, so the file only contributes the keys the block does not regenerate
        auto restoreMetaInformation = [&grcBlock](BlockModel& createdBlock) {
            const auto metaIt = grcBlock.find("meta_information");
            if (metaIt == grcBlock.cend()) {
                return;
            }
            const auto meta = checked_access_ptr<const property_map, false>{metaIt->second.get_if<property_map>()};
            if (meta == nullptr) {
                return;
            }
            for (const auto& [key, value] : *meta) {
                createdBlock.metaInformation().try_emplace(key, value);
            }
        };

        if (isSubgraph) {
            auto loadGraph = [&grcBlock, &loader, &location, &blockName, &blockType](auto graphWrapper) {
                // checked_access_ptr terminates on a null unless not_null is turned off, so the
                // non-terminating form is what keeps the report below reachable: a subgraph whose
                // graph field is present but not a map is a defect in the document and is named as one
                const auto _graphData = checked_access_ptr<const property_map, false>{grcBlock.at("graph").get_if<property_map>()};
                if (_graphData == nullptr) {
                    throw gr::exception(std::format("Unable to create block '{}' of type '{}': graph is not a map", blockName, blockType));
                }
                const auto&        graphData    = *_graphData;
                gr::Graph&         graph        = *graphWrapper->graph();
                const LoadedBlocks innerBlocks  = loadGraphFromMap(loader, graph, graphData);
                const auto         exportedIt   = graphData.find("exported_ports");
                const auto         exportedList = exportedIt == graphData.cend() ? Tensor<pmt::Value>() : exportedIt->second.value_or(Tensor<pmt::Value>());
                for (const auto& exportedPort_ : exportedList) {
                    auto exportedPort = checked_access_ptr<const Tensor<pmt::Value>, false>{exportedPort_.get_if<Tensor<pmt::Value>>()};
                    if (exportedPort == nullptr) {
                        throw gr::exception("Unable to parse exported port (not a list)");
                    }
                    if (exportedPort->size() != 4) {
                        throw gr::exception(std::format("Unable to parse exported port ({} instead of 4 elements)", exportedPort->size()));
                    }

                    const auto requiredBlockName   = (*exportedPort)[0].value_or(std::string_view{});
                    const auto portDirectionString = (*exportedPort)[1].value_or(std::string_view{});
                    const auto internalPortName    = (*exportedPort)[2].value_or(std::string_view{});
                    const auto exportedPortName    = (*exportedPort)[3].value_or(std::string_view{});
                    if (requiredBlockName.data() == nullptr || portDirectionString.data() == nullptr || internalPortName.data() == nullptr || exportedPortName.data() == nullptr) {
                        throw gr::exception(std::format("Required fields for exported ports missing"));
                    }

                    // the writer names the inner block by unique_name, a hand-written file by name
                    const auto innerBlock = innerBlocks.find(requiredBlockName);
                    if (!innerBlock.has_value()) {
                        throw gr::exception(std::format("{} in:\n{}", innerBlock.error().message, gr::graph::format(graph)), location);
                    }
                    const std::string innerUniqueName{innerBlock.value()->uniqueName()};

                    if (auto result = graphWrapper->exportPort(true,                                       //
                            innerUniqueName,                                                               //
                            portDirectionString == "INPUT" ? PortDirection::INPUT : PortDirection::OUTPUT, //
                            internalPortName,                                                              //
                            exportedPortName);
                        !result.has_value()) {
                        throw result.error();
                    }
                }
            };

            auto       schedulerIt = grcBlock.find("scheduler");
            const bool isManaged   = schedulerIt != grcBlock.end();

            if (isManaged) {
                auto schedulerPmt = checked_access_ptr<const property_map, false>{schedulerIt->second.get_if<property_map>()};
                if (schedulerPmt == nullptr) {
                    throw gr::exception(std::format("scheduler is not a property_map"));
                }
                auto schedulerId = getOrThrow(getProperty<std::string>(*schedulerPmt, "id"sv));

                property_map schedulerParams;
                if (auto paramsIt = schedulerPmt->find("parameters"); paramsIt != schedulerPmt->end()) {
                    if (const auto params = checked_access_ptr<const property_map, false>{paramsIt->second.get_if<property_map>()}; params != nullptr) {
                        schedulerParams = *params;
                    }
                }

                auto scheduler = loader.instantiateScheduler(schedulerId, schedulerParams);
                if (!scheduler) {
                    throw gr::exception(std::format("Unable to create scheduler of type '{}'", schedulerId));
                }

                auto schedulerBlock = SchedulerModel::asBlockModelPtr(scheduler);
                resultGraph.addBlock(schedulerBlock);
                schedulerBlock->setName(blockName);
                restoreMetaInformation(*schedulerBlock);
                createdBlocks.add(blockUniqueName, blockName, schedulerBlock);

                loadGraph(schedulerBlock);

            } else {
                const std::shared_ptr<BlockModel>& subGraph = resultGraph.addBlock(std::make_shared<GraphWrapper<gr::Graph>>(gr::Graph(loader)));
                subGraph->setName(blockName);
                restoreMetaInformation(*subGraph);
                createdBlocks.add(blockUniqueName, blockName, subGraph);

                loadGraph(static_cast<GraphWrapper<gr::Graph>*>(subGraph.get()));
            }
        } else {
            const auto          parametersPmt = grcBlock.at("parameters");
            const property_map* parameters    = parametersPmt.get_if<property_map>();
            const property_map* given         = settingsByPosition[position];
            const property_map  merged        = mergedParameters(parameters, given);

            auto currentBlock = loader.instantiate(blockType, merged);
            if (!currentBlock) {
                throw gr::exception(std::format("Unable to create block of type '{}'", blockType));
            }
            // the settings take the map once, below. Settings::init() would apply the constructor's copy again and
            // refuse a key of the file the block does not declare.
            currentBlock->settings().setInitBlockParameters({});
            if (given != nullptr) {
                checkDeclared(*currentBlock, blockName, *given);
            }

            // This sets the previously read "name" field for the block
            currentBlock->setName(blockName);

            currentBlock->settings().loadParametersFromPropertyMap(merged);

            if (auto it = grcBlock.find("ctx_parameters"); it != grcBlock.end()) {
                // as with the graph field above, the null tests below are reachable only because the
                // pointers they test are the non-terminating form
                const auto parametersCtx = checked_access_ptr<const Tensor<pmt::Value>, false>{it->second.get_if<Tensor<pmt::Value>>()};
                if (parametersCtx == nullptr) {
                    throw gr::exception(std::format("Unable to create block '{}' of type '{}': ctx_parameters is not a list", blockName, blockType));
                }

                for (const auto& ctxPmt : *parametersCtx) {
                    const auto ctxPar = checked_access_ptr<const property_map, false>{ctxPmt.get_if<property_map>()};
                    if (ctxPar == nullptr) {
                        throw gr::exception(std::format("Unable to create block '{}' of type '{}': a ctx_parameters entry is not a map", blockName, blockType));
                    }

                    const auto ctxName       = ctxPar->at(gr::tag::CONTEXT.shortKey()).value_or(std::string_view{});
                    const auto ctxTime       = checked_access_ptr<const std::uint64_t, false>{ctxPar->at(gr::tag::CONTEXT_TIME.shortKey()).get_if<std::uint64_t>()};
                    const auto ctxParameters = checked_access_ptr<const property_map, false>{ctxPar->at("parameters").get_if<property_map>()};
                    if (ctxName.data() == nullptr || ctxTime == nullptr || ctxParameters == nullptr) {
                        throw gr::exception(std::format("Unable to create block '{}' of type '{}': a ctx_parameters entry needs a context, a context_time and a parameters map", blockName, blockType));
                    }

                    currentBlock->settings().loadParametersFromPropertyMap(*ctxParameters, SettingsCtx{*ctxTime, ctxName});
                }
            }

            if (const auto failed = currentBlock->settings().activateContext(); failed == std::nullopt) {
                throw gr::exception("Settings for context could not be activated");
            }

            restoreMetaInformation(*currentBlock);
            createdBlocks.add(blockUniqueName, blockName, resultGraph.addBlock(std::move(currentBlock)));
        }
    } // for blocks

    Tensor<pmt::Value> connections;
    if (auto it = yaml.find("connections"); it != yaml.end()) {
        if (const auto connRef = checked_access_ptr<Tensor<pmt::Value>, false>{it->second.get_if<Tensor<pmt::Value>>()}; connRef != nullptr) {
            connections = *connRef;
        }
    }

    for (const auto& conn : connections) {
        const auto _connection = checked_access_ptr<const Tensor<pmt::Value>, false>{conn.get_if<Tensor<pmt::Value>>()};
        if (_connection == nullptr) {
            throw gr::exception("Unable to parse connection (not a list)");
        }
        if (_connection->size() < 4) {
            throw gr::exception(std::format("Unable to parse connection ({} instead of >=4 elements)", _connection->size()));
        }
        const auto& connection = *_connection;

        auto parseBlockPort = [&](const pmt::Value& blockField, const pmt::Value& portField) {
            const auto blockName = blockField.value_or(std::string_view{});
            if (blockName.empty()) {
                throw gr::exception(std::format("Invalid blockField"));
            }
            auto block = createdBlocks.find(blockName);
            if (!block.has_value()) {
                throw gr::exception(block.error().message);
            }

            struct result {
                std::shared_ptr<BlockModel> block;
                PortDefinition              port_definition;
            };

            if (const auto portFields = checked_access_ptr<const Tensor<pmt::Value>, false>{portField.template get_if<Tensor<pmt::Value>>()}; portFields != nullptr) {
                if (portFields->size() != 2) {
                    throw gr::exception(std::format("Port definition has invalid length ({} instead of 2)", portFields->size()));
                }
                const auto index    = checked_access_ptr<const std::int64_t, false>{portFields->at(0).template get_if<std::int64_t>()};
                const auto subIndex = checked_access_ptr<const std::int64_t, false>{portFields->at(1).template get_if<std::int64_t>()};
                if (index == nullptr || subIndex == nullptr) {
                    throw gr::exception(std::format("Port definition missing values"));
                }

                return result{*block, {static_cast<std::size_t>(*index), static_cast<std::size_t>(*subIndex)}};

            } else if (const auto portFieldString = portField.value_or(std::string_view{}); portFieldString.data()) {
                return result{*block, {std::string(portFieldString)}};

            } else {
                const auto index = checked_access_ptr<const std::int64_t, false>{portField.template get_if<std::int64_t>()};
                if (index == nullptr) {
                    throw gr::exception(std::format("Port definition missing values"));
                }
                return result{*block, {static_cast<std::size_t>(*index)}};
            }
        };

        auto src = parseBlockPort(connection[0], connection[1]);
        auto dst = parseBlockPort(connection[2], connection[3]);

        // a file states a whole graph: two connections into one stream input are an error in the file
        const Edge                  requested(src.block, src.port_definition, dst.block, dst.port_definition, EdgeParameters{});
        const std::span<const Edge> loadedEdges = resultGraph.edges();
        if (const auto taken = std::ranges::find_if(loadedEdges, [&requested](const Edge& edge) { return edge.hasSameStreamInput(requested); }); taken != loadedEdges.end()) {
            throw gr::exception(std::format("stream input {}/{} is the destination of two connections, from {}/{} and from {}/{}", dst.block->name(), dst.port_definition, taken->sourceBlock()->name(), taken->sourcePortDefinition(), src.block->name(), src.port_definition));
        }

        if (connection.size() == 4) {
            if (auto r = resultGraph.connect(src.block, src.port_definition, dst.block, dst.port_definition, EdgeParameters{.minBufferSize = undefined_size, .weight = graph::defaultWeight, .name = graph::defaultEdgeName}, location); !r) {
                throw gr::exception(std::format("connection failed: {}", r.error().message));
            }
        } else {
            std::size_t minBufferSize{};
            pmt::ValueVisitor([&minBufferSize]<typename TValue>(const TValue& value) {
                if constexpr (std::is_same_v<TValue, std::size_t>) {
                    minBufferSize = value;
                } else if constexpr (std::is_integral_v<TValue>) {
                    minBufferSize = static_cast<std::size_t>(value);
                } else {
                    minBufferSize = std::numeric_limits<std::size_t>::max();
                }
            }).visit(connection[4]);

            if (auto r = resultGraph.connect(src.block, src.port_definition, dst.block, dst.port_definition, EdgeParameters{.minBufferSize = minBufferSize, .weight = graph::defaultWeight, .name = graph::defaultEdgeName}, location); !r) {
                throw gr::exception(std::format("connection failed: {}", r.error().message));
            }
        }
    } // for connections

    return createdBlocks;
}

inline gr::property_map saveGraphToMap(PluginLoader& loader, const gr::Graph& rootGraph) {
    property_map result;

    {
        const std::size_t  nBlocks = gr::graph::countBlocks<gr::block::Category::NormalBlock>(rootGraph);
        Tensor<pmt::Value> serializedBlocks;
        serializedBlocks.reserve(nBlocks);
        gr::graph::forEachBlock<gr::block::Category::NormalBlock>(rootGraph, [&serializedBlocks, &loader](const std::shared_ptr<BlockModel>& block) { serializedBlocks.emplace_back(serializeBlock(loader, block, BlockSerializationFlags::All & (~BlockSerializationFlags::Ports))); });
        result["blocks"] = std::move(serializedBlocks);
    }

    {
        const std::size_t  nEdges = gr::graph::countEdges<block::Category::NormalBlock>(rootGraph);
        Tensor<pmt::Value> serializedConnections;
        serializedConnections.reserve(nEdges);
        graph::forEachEdge<block::Category::NormalBlock>(rootGraph, [&serializedConnections](const Edge& edge) { // NormalBlock -> perhaps can be modelled to 'ALL' for a cleaner sub-graph handling
            Tensor<pmt::Value> seq;
            seq.reserve(7);

            auto writePortDefinition = [&](const auto& definition) {
                if (auto* idx = std::get_if<PortDefinition::IndexBased>(&definition.definition)) {
                    if (idx->subIndex != meta::invalid_index) {
                        Tensor<pmt::Value> seqPort;
                        seqPort.reserve(2);
                        seqPort.push_back(std::int64_t(idx->topLevel));
                        seqPort.push_back(std::int64_t(idx->subIndex));
                        seq.push_back(std::move(seqPort));
                    } else {
                        seq.push_back(std::int64_t(idx->topLevel));
                    }
                } else {
                    auto& str = std::get<PortDefinition::StringBased>(definition.definition);
                    seq.push_back(str.name);
                }
            };

            // an edge names its ends by unique_name: a name may repeat, and two blocks sharing one lose an edge on load
            seq.push_back(std::string(edge.sourceBlock()->uniqueName()));
            writePortDefinition(edge.sourcePortDefinition());

            seq.push_back(std::string(edge.destinationBlock()->uniqueName()));
            writePortDefinition(edge.destinationPortDefinition());

            if (edge.minBufferSize() != std::numeric_limits<std::size_t>::max()) {
                seq.push_back(static_cast<gr::Size_t>(edge.minBufferSize()));
            }

            serializedConnections.emplace_back(std::move(seq));
        });
        result["connections"] = std::move(serializedConnections);
    }

    return result;
}

} // namespace detail

/// Reads a graph from a GRC document, with `overrides` merged over the parameters of the top-level blocks its keys name
/// (see `BlockSettings`). A key no block carries, a key two blocks carry and a key a block does not declare are
/// refused.
inline gr::meta::indirect<gr::Graph> loadGrc(PluginLoader& loader, std::string_view yamlSrc, const BlockSettings& overrides = {}, std::source_location location = std::source_location::current()) {
    gr::meta::indirect<gr::Graph> resultGraph{loader};
    const auto                    yaml = pmt::yaml::deserialize(yamlSrc);
    if (!yaml) {
        throw gr::exception(std::format("Could not parse yaml: {}:{}\n{}", yaml.error().message, yaml.error().line, yamlSrc));
    }

    detail::loadGraphFromMap(loader, *resultGraph, *yaml, overrides, location);
    return resultGraph;
}

/// The key order a GRC document is emitted in: identity first, structure last, so a reader
/// skimming a block sees what it is before how it is configured. Every map in the document is
/// emitted with these keys first and the remainder lexicographically, which also makes the
/// output deterministic (the underlying map's own iteration order is a hash artifact).
inline constexpr std::array<std::string_view, 12> grcYamlKeyOrder{"id", "name", "unique_name", "block_category", "meta_information", "parameters", "ctx_parameters", "scheduler", "exported_ports", "blocks", "connections", "graph"};

inline std::string saveGrc(PluginLoader& loader, const gr::Graph& rootGraph) { return pmt::yaml::serialize(detail::saveGraphToMap(loader, rootGraph), grcYamlKeyOrder); }

inline std::expected<std::shared_ptr<gr::BlockModel>, gr::Error> detail::instantiateBlockFromYamlDefinition(PluginLoader& loader, const detail::YamlDefinitionsLoader::Definition& def) noexcept {
    try {
        gr::Graph tempGraph;
        detail::loadGraphFromMap(loader, tempGraph, def.definition);
        auto blocks = tempGraph.blocks();
        if (blocks.empty()) {
            return std::unexpected(gr::Error{"YAML definition produced no blocks"});
        }
        return blocks.front();
    } catch (const gr::exception& e) {
        return std::unexpected(gr::Error{e});
    } catch (const std::exception& e) {
        return std::unexpected(gr::Error{e});
    }
}

} // namespace gr

#endif // include guard
