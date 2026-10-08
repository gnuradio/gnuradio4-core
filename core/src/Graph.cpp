#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>

namespace gr {

Graph::Graph(property_map settings) : gr::Block<Graph>(std::move(settings)), _pluginLoader(std::addressof(gr::globalPluginLoader())) {
    _blocks.reserve(100); // TODO: remove

    propertyCallbacks[graph::property::kInspectBlock]           = static_cast<BlockBase::PropertyCallback>(&Graph::propertyCallbackInspectBlock);
    propertyCallbacks[graph::property::kGraphInspect]           = static_cast<BlockBase::PropertyCallback>(&Graph::propertyCallbackGraphInspect);
    propertyCallbacks[graph::property::kRegistryBlockTypes]     = static_cast<BlockBase::PropertyCallback>(&Graph::propertyCallbackRegistryBlockTypes);
    propertyCallbacks[graph::property::kRegistrySchedulerTypes] = static_cast<BlockBase::PropertyCallback>(&Graph::propertyCallbackRegistrySchedulerTypes);
}

[[maybe_unused]] std::shared_ptr<BlockModel> const& Graph::emplaceBlock(std::string_view type, property_map initialSettings) {
    if (type.starts_with("gr::Graph")) {
        // the nested graph inherits this graph's plugin loader, so blocks resolvable here stay
        // resolvable inside the subgraph
        auto subGraphModel = std::unique_ptr<BlockModel>(std::make_unique<GraphWrapper<Graph>>(Graph(*_pluginLoader, std::move(initialSettings))).release());
        return addBlock(std::move(subGraphModel));
    } else if (std::shared_ptr<BlockModel> block_load = _pluginLoader->instantiate(type, initialSettings); block_load) {
        const std::shared_ptr<BlockModel>& newBlock = addBlock(block_load);
        return newBlock;
    } else if (std::shared_ptr<SchedulerModel> scheduler_load = _pluginLoader->instantiateScheduler(type, initialSettings); scheduler_load) {
        const std::shared_ptr<BlockModel>& newBlock = addBlock(SchedulerModel::asBlockModelPtr(scheduler_load));
        return newBlock;
    }
    throw gr::exception(std::format("Cannot create block '{}'", type));
}

std::pair<std::shared_ptr<BlockModel>, std::shared_ptr<BlockModel>> Graph::replaceBlock(std::string_view uniqueName, std::string_view type, const property_map& properties) {
    auto found = std::ranges::find_if(_blocks, [&uniqueName](const auto& block) { return block->uniqueName() == uniqueName; });
    if (found == _blocks.end()) {
        throw gr::exception(std::format("Block {} was not found in {}", uniqueName, this->unique_name));
    }
    if (std::ranges::any_of(_exportedPorts, [&uniqueName](const ExportedPort& exported) { return exported.blockName == uniqueName; })) {
        throw gr::exception(std::format("Block {} in {} exports a port and cannot be replaced", uniqueName, this->unique_name));
    }
    const std::shared_ptr<BlockModel> replaced = *found;

    std::shared_ptr<BlockModel> newBlock = _pluginLoader->instantiate(type, properties);
    if (!newBlock) {
        throw gr::exception(std::format("Can not create block {}", type));
    }
    newBlock->init(_progress, this->compute_domain); // a setting can size a port collection

    // Each port of the replaced block that an edge names pairs with one port of the new block. All pairs are checked
    // before any buffer moves.
    struct PortPair {
        DynamicPort* replacedPort;
        DynamicPort* newPort;
    };
    std::vector<PortPair> pairs;
    auto                  pairPort = [&](const PortDefinition& definition, bool isOutput) {
        auto replacedPort = isOutput ? replaced->dynamicOutputPort(definition) : replaced->dynamicInputPort(definition);
        auto newPort      = isOutput ? newBlock->dynamicOutputPort(definition) : newBlock->dynamicInputPort(definition);
        if (!replacedPort || !newPort) {
            throw gr::exception(std::format("Block {} cannot replace {} in {}: {}", type, uniqueName, this->unique_name, (!newPort ? newPort.error() : replacedPort.error()).message));
        }
        if (auto exchange = (*replacedPort)->checkHandlerExchange(**newPort); !exchange) {
            throw gr::exception(std::format("Block {} cannot replace {} in {}: {}", type, uniqueName, this->unique_name, exchange.error().message));
        }
        for (const PortPair& pair : pairs) {
            if ((pair.replacedPort == *replacedPort) != (pair.newPort == *newPort)) {
                throw gr::exception(std::format("Block {} cannot replace {} in {}: two edges name one port on one block and two ports on the other", type, uniqueName, this->unique_name));
            }
        }
        if (std::ranges::none_of(pairs, [&replacedPort](const PortPair& pair) { return pair.replacedPort == *replacedPort; })) {
            pairs.push_back({*replacedPort, *newPort});
        }
        return *newPort;
    };

    struct EdgePorts {
        std::size_t  index;
        DynamicPort* source;
        DynamicPort* destination;
    };
    std::vector<EdgePorts> takenOver;
    for (std::size_t index = 0UZ; index < _edges.size(); ++index) {
        const Edge& edge = _edges[index];
        if (edge._sourceBlock != replaced && edge._destinationBlock != replaced) {
            continue;
        }
        DynamicPort* source      = edge._sourceBlock == replaced ? pairPort(edge._sourcePortDefinition, true) : edge._sourcePort;
        DynamicPort* destination = edge._destinationBlock == replaced ? pairPort(edge._destinationPortDefinition, false) : edge._destinationPort;
        takenOver.push_back({index, source, destination});
    }

    for (const PortPair& pair : pairs) {
        pair.replacedPort->exchangeHandlers(*pair.newPort);
    }
    for (const EdgePorts& ports : takenOver) {
        Edge& edge = _edges[ports.index];
        if (edge._sourceBlock == replaced) {
            edge._sourceBlock = newBlock;
            edge._sourcePort  = ports.source;
        }
        if (edge._destinationBlock == replaced) {
            edge._destinationBlock = newBlock;
            edge._destinationPort  = ports.destination;
        }
    }
    *found = newBlock;
    sizeUnconnectedOptionalOutputs(*newBlock);

    return {replaced, newBlock};
}

std::optional<Message> Graph::propertyCallbackRegistryBlockTypes([[maybe_unused]] std::string_view propertyName, Message message) {
    assert(propertyName == graph::property::kRegistryBlockTypes);
    const auto&        availableBlocks = _pluginLoader->availableBlocks();
    Tensor<pmt::Value> types(availableBlocks | std::views::transform([](const std::string& type) { return pmt::Value(type); }));
    message.data = property_map{{"types", types}};
    return message;
}

std::optional<Message> Graph::propertyCallbackRegistrySchedulerTypes([[maybe_unused]] std::string_view propertyName, Message message) {
    assert(propertyName == graph::property::kRegistrySchedulerTypes);
    const auto&        availableSchedulers = _pluginLoader->availableSchedulers();
    Tensor<pmt::Value> types(availableSchedulers | std::views::transform([](const std::string& type) { return pmt::Value(type); }));
    message.data = property_map{{"types", types}};
    return message;
}

std::optional<Message> Graph::propertyCallbackInspectBlock([[maybe_unused]] std::string_view propertyName, Message message) {
    assert(propertyName == graph::property::kInspectBlock);
    using namespace std::string_literals;

    gr::Message reply;
    reply.endpoint = graph::property::kBlockInspected;

    if (!message.data) {
        reply.data = std::unexpected(Error{"Invalid block specification"s});
        return reply;
    }
    const auto& data       = *message.data;
    const auto  uniqueName = data.at("uniqueName").value_or(std::string_view{});
    if (uniqueName.empty()) {
        reply.data = std::unexpected(Error{"Invalid block specification"s});
        return reply;
    }

    auto it = std::ranges::find_if(_blocks, [&uniqueName](const auto& block) { return block->uniqueName() == uniqueName; });
    if (it == _blocks.end()) {
        reply.data = std::unexpected(Error{std::format("Block {} was not found in {}", uniqueName, this->unique_name)});
        return reply;
    }

    const bool yamlSerialize = [&] {
        if (const auto fmt = data.find("serialization_format"); fmt != data.cend()) {
            return fmt->second == "yaml";
        }
        return false;
    }();

    if (yamlSerialize) {
        reply.data = property_map{{"yamlData", pmt::yaml::serialize(serializeBlock(*_pluginLoader, *it, BlockSerializationFlags::All))}};
    } else {
        reply.data = serializeBlock(*_pluginLoader, *it, BlockSerializationFlags::All);
    }
    return {reply};
}

std::optional<Message> Graph::propertyCallbackGraphInspect([[maybe_unused]] std::string_view propertyName, Message message) {
    assert(propertyName == graph::property::kGraphInspect);

    if (const bool yamlSerialize =
            [&] {
                if (!message.data) {
                    return false;
                }
                if (const auto it = message.data->find("serialization_format"); it != message.data->cend()) {
                    return it->second == "yaml";
                }
                return false;
            }();
        !yamlSerialize) {
        message.data = [&] {
            property_map _result;
            auto&        result = _result;

            result[std::pmr::string(serialization_fields::BLOCK_NAME)]        = std::string(name);
            result[std::pmr::string(serialization_fields::BLOCK_UNIQUE_NAME)] = std::string(unique_name);
            result[std::pmr::string(serialization_fields::BLOCK_CATEGORY)]    = std::string(gr::meta::enumName(blockCategory).value_or(""));

            property_map serializedChildren;
            for (const auto& child : blocks()) {
                serializedChildren[std::pmr::string(child->uniqueName())] = serializeBlock(*_pluginLoader, child, BlockSerializationFlags::All);
            }
            result[std::pmr::string(serialization_fields::BLOCK_CHILDREN)] = std::move(serializedChildren);

            property_map serializedEdges;
            std::size_t  index = 0UZ;
            for (const auto& edge : edges()) {
                serializedEdges[convert_string_domain(std::to_string(index))] = serializeEdge(edge);
                index++;
            }
            result[std::pmr::string(serialization_fields::BLOCK_EDGES)] = std::move(serializedEdges);
            return result;
        }();
    } else {
        message.data = {{"yamlData", saveGrc(*_pluginLoader, *this)}};
    }

    message.endpoint = graph::property::kGraphInspected;
    return message;
}
} // namespace gr
