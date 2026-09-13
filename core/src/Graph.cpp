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
    // Exported aliases live on the enclosing wrapper, outside this graph's edge list.
    // Replacing their owner requires an explicit unexport first.
    for (const auto key : {"exportedInputPorts", "exportedOutputPorts"}) {
        if (auto it = meta_information.value.find(key); it != meta_information.value.end()) {
            if (const auto* exports = it->second.get_if<property_map>(); exports && exports->contains(std::pmr::string(uniqueName))) {
                throw gr::exception(std::format("Cannot replace block {} while its ports are exported", uniqueName));
            }
        }
    }
    const std::shared_ptr<BlockModel> replaced = *found;
    auto newBlock = _pluginLoader->instantiate(type, properties);
    if (!newBlock) {
        throw gr::exception(std::format("Can not create block {}", type));
    }
    // Initialise before looking up ports: settings can determine a collection's size.
    newBlock->init(_progress, this->compute_domain);
    if (auto error = newBlock->initError()) {
        throw gr::exception(error->message);
    }

    struct ReplacementEdge {
        std::size_t index;
        Edge replacement;
    };
    std::vector<ReplacementEdge> changes;
    std::vector<std::pair<DynamicPort*, DynamicPort*>> bindings;
    auto validateBinding = [&](DynamicPort* before, DynamicPort* after) {
        if (before->domain() != after->domain() || before->bufferSize() < after->min_samples) {
            throw gr::exception("Replacement port cannot use the existing buffer domain or capacity");
        }
        for (const auto& [oldPort, newPort] : bindings) {
            if ((oldPort == before) != (newPort == after)) {
                throw gr::exception("Replacement port definitions do not preserve distinct endpoints");
            }
        }
        if (std::ranges::find(bindings, std::pair{before, after}) == bindings.end()) {
            bindings.emplace_back(before, after);
        }
    };
    for (std::size_t i = 0UZ; i < _edges.size(); ++i) {
        const Edge& edge = _edges[i];
        if (edge._sourceBlock != replaced && edge._destinationBlock != replaced) {
            continue;
        }
        Edge next = edge;
        if (next._sourceBlock == replaced) {
            next._sourceBlock = newBlock;
        }
        if (next._destinationBlock == replaced) {
            next._destinationBlock = newBlock;
        }
        auto source = next._sourceBlock->dynamicOutputPort(next._sourcePortDefinition);
        auto destination = next._destinationBlock->dynamicInputPort(next._destinationPortDefinition);
        if (!source || !destination) {
            throw gr::exception((!source ? source.error() : destination.error()).message);
        }
        if ((*source)->typeName() != (*destination)->typeName() ||
            port::decodePortType((*source)->portMaskInfo()) != port::decodePortType((*destination)->portMaskInfo()) ||
            port::decodeDirection((*source)->portMaskInfo()) != PortDirection::OUTPUT ||
            port::decodeDirection((*destination)->portMaskInfo()) != PortDirection::INPUT) {
            throw gr::exception(std::format("Incompatible replacement ports for edge {}", edge));
        }
        auto oldSource = edge._sourceBlock->dynamicOutputPort(edge._sourcePortDefinition);
        auto oldDestination = edge._destinationBlock->dynamicInputPort(edge._destinationPortDefinition);
        if (!oldSource || !oldDestination) {
            throw gr::exception((!oldSource ? oldSource.error() : oldDestination.error()).message);
        }
        // Mixed name/index definitions must neither merge distinct ports nor split a fan-out.
        if (edge._sourceBlock == replaced) {
            validateBinding(*oldSource, *source);
        }
        if (edge._destinationBlock == replaced) {
            validateBinding(*oldDestination, *destination);
        }
        // Connectivity belongs to this edge, not to either port independently. A pending
        // edge can name an output and input that are each connected to different peers.
        // emplaceEdge() records Connected after its specific connect() succeeds.
        const bool connected = edge.state() == Edge::EdgeState::Connected;
        next._sourcePort = *source;
        next._destinationPort = *destination;
        next._state = connected ? Edge::EdgeState::Connected : Edge::EdgeState::WaitingToBeConnected;
        if (connected) {
            next._actualBufferSize = (*oldSource)->bufferSize();
            next._edgeType = port::decodePortType((*source)->portMaskInfo());
        }
        changes.push_back({i, std::move(next)});
    }

    // Transfer the actual registrations, rings and sample/tag cursors. Reconnecting via
    // new_reader() would attach at the publish cursor and discard pending data and EOS.
    // In particular downstream fan-out readers must retain their independent positions.
    std::vector<std::function<void()>> exchanges;
    exchanges.reserve(bindings.size());
    for (auto [before, after] : bindings) {
        exchanges.push_back(before->prepareBufferExchange(*after));
    }
    for (auto& exchange : exchanges) { exchange(); }
    try {
        if (auto result = resizeOptionalOutputs(newBlock); !result) {
            throw gr::exception(result.error().message);
        }
    } catch (...) {
        // No reader is recreated: reversing the exchange restores exact unread history.
        for (auto& exchange : exchanges) { exchange(); }
        throw;
    }
    for (auto& change : changes) {
        Edge& edge = _edges[change.index];
        edge = std::move(change.replacement);
        if (!edge._domainStr.empty()) {
            edge._domain = ComputeDomain::parse(edge._domainStr);
        }
    }

    *found = newBlock;
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
