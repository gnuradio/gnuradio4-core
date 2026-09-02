#ifndef GNURADIO_GRAPH_YAML_IMPORTER_H
#define GNURADIO_GRAPH_YAML_IMPORTER_H

#include <array>
#include <map>
#include <ranges>
#include <set>

#include <gnuradio-4.0/meta/indirect.hpp>

#include <gnuradio-4.0/YamlPmt.hpp>

#include "BlockModel.hpp"
#include "Graph.hpp"
#include "PluginLoader.hpp"

namespace gr {

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
        auto value = checked_access_ptr{it->second.get_if<T>()};
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

    auto value = checked_access_ptr{it->second.get_if<gr::property_map>()};
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

inline LoadedBlocks loadGraphFromMap(PluginLoader& loader, gr::Graph& resultGraph, gr::property_map yaml, std::source_location location = std::source_location::current()) {
    LoadedBlocks createdBlocks;

    Tensor<pmt::Value> blks;
    if (auto it = yaml.find("blocks"); it != yaml.end()) {
        if (const auto blkRef = checked_access_ptr<Tensor<pmt::Value>, false>{it->second.get_if<Tensor<pmt::Value>>()}; blkRef != nullptr) {
            blks = *blkRef;
        }
    }

    for (const auto& blk : blks) {
        const auto _grcBlock = checked_access_ptr{blk.get_if<property_map>()};
        if (_grcBlock == nullptr) {
            continue;
        }
        const auto& grcBlock = *_grcBlock;

        // the type decides which fields are required, so it is read first
        const auto blockType       = getOrThrow(getProperty<std::string>(grcBlock, "id"sv));
        const bool isSubgraph      = blockType == "SUBGRAPH";
        const auto blockUniqueName = getProperty<std::string>(grcBlock, "unique_name"sv).value_or(std::string{});
        const auto blockName       = [&] {
            auto fromParameters = getProperty<std::string>(grcBlock, "parameters"sv, "name"sv);
            if (fromParameters.has_value() || !isSubgraph) {
                return getOrThrow(std::move(fromParameters));
            }
            // subgraphs written before the parameters key carry their name at the top level
            return getProperty<std::string>(grcBlock, "name"sv).value_or(std::string{});
        }();

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
                    auto exportedPort = checked_access_ptr{exportedPort_.get_if<Tensor<pmt::Value>>()};
                    if (exportedPort == nullptr || exportedPort->size() != 4) {
                        throw gr::exception(std::format("Unable to parse exported port ({} instead of 4 elements)", exportedPort != nullptr ? exportedPort->size() : -1UZ));
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
                auto schedulerPmt = checked_access_ptr{schedulerIt->second.get_if<property_map>()};
                if (schedulerPmt == nullptr) {
                    throw gr::exception(std::format("scheduler is not a property_map"));
                }
                auto schedulerId = getOrThrow(getProperty<std::string>(*schedulerPmt, "id"sv));

                property_map schedulerParams;
                if (auto paramsIt = schedulerPmt->find("parameters"); paramsIt != schedulerPmt->end()) {
                    if (const auto params = checked_access_ptr{paramsIt->second.get_if<property_map>()}; params != nullptr) {
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
            auto currentBlock = loader.instantiate(blockType);
            if (!currentBlock) {
                throw gr::exception(std::format("Unable to create block of type '{}'", blockType));
            }

            // This sets the previously read "name" field for the block
            currentBlock->setName(blockName);

            const auto parametersPmt = grcBlock.at("parameters");
            if (const auto parameters = checked_access_ptr{parametersPmt.get_if<property_map>()}; parameters != nullptr) {
                currentBlock->settings().loadParametersFromPropertyMap(*parameters);
            } else {
                currentBlock->settings().loadParametersFromPropertyMap({});
            }

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
        const auto _connection = checked_access_ptr{conn.get_if<Tensor<pmt::Value>>()};
        if (_connection == nullptr || _connection->size() < 4) {
            throw gr::exception(std::format("Unable to parse connection ({} instead of >=4 elements)", _connection == nullptr ? -1UZ : _connection->size()));
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
                const auto index    = checked_access_ptr{portFields->at(0).template get_if<std::int64_t>()};
                const auto subIndex = checked_access_ptr{portFields->at(1).template get_if<std::int64_t>()};
                if (index == nullptr || subIndex == nullptr) {
                    throw gr::exception(std::format("Port definition missing values"));
                }

                return result{*block, {static_cast<std::size_t>(*index), static_cast<std::size_t>(*subIndex)}};

            } else if (const auto portFieldString = portField.value_or(std::string_view{}); portFieldString.data()) {
                return result{*block, {std::string(portFieldString)}};

            } else {
                const auto index = checked_access_ptr{portField.template get_if<std::int64_t>()};
                if (index == nullptr) {
                    throw gr::exception(std::format("Port definition missing values"));
                }
                return result{*block, {static_cast<std::size_t>(*index)}};
            }
        };

        auto src = parseBlockPort(connection[0], connection[1]);
        auto dst = parseBlockPort(connection[2], connection[3]);

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

inline gr::meta::indirect<gr::Graph> loadGrc(PluginLoader& loader, std::string_view yamlSrc, std::source_location location = std::source_location::current()) {
    gr::meta::indirect<gr::Graph> resultGraph{loader};
    const auto                    yaml = pmt::yaml::deserialize(yamlSrc);
    if (!yaml) {
        throw gr::exception(std::format("Could not parse yaml: {}:{}\n{}", yaml.error().message, yaml.error().line, yamlSrc));
    }

    detail::loadGraphFromMap(loader, *resultGraph, *yaml, location);
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
