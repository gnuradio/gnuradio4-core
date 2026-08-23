#include <string>

#include <gnuradio-4.0/BlockModel.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>

namespace gr {

std::string_view BlockModel::typeName() const { return _typeName; }

gr::Graph* BlockModel::graph() { return nullptr; }

gr::property_map BlockModel::exportedInputPorts() { return {}; }

gr::property_map BlockModel::exportedOutputPorts() { return {}; }

std::expected<void, Error> BlockModel::exportPort(bool, std::string_view, PortDirection, std::string_view, std::string_view, std::source_location) { return {}; }

property_map serializeBlockImpl(gr::PluginLoader& pluginLoader, const std::shared_ptr<BlockModel>& block, int flags) {
    using namespace std::string_literals;

    property_map result;
    result.emplace(serialization_fields::BLOCK_ID, pluginLoader.registry().typeName(block));
    result.emplace(serialization_fields::BLOCK_UNIQUE_NAME, std::string(block->uniqueName()));
    result.emplace(serialization_fields::BLOCK_CATEGORY, std::string(gr::meta::enumName(block->blockCategory()).value_or("")));

    if (!block->metaInformation().empty()) {
        result.emplace(serialization_fields::BLOCK_META_INFORMATION, block->metaInformation());
    }

    if (flags & BlockSerializationFlags::Settings) {
        // Helper function to write parameters
        auto writeParameters = [&](const property_map& settingsMap) {
            pmt::Value::Map parameters;
            auto            writeMap = [&](const auto& localMap) {
                for (const auto& [settingsKey, settingsValue] : localMap) {
                    parameters[settingsKey] = settingsValue;
                }
            };
            writeMap(settingsMap);
            return parameters;
        };

        // Serialization must not change the graph it serializes, so the staged parameters are read rather
        // than committed: the dump carries what the block would use, and the block keeps them staged.
        //
        // Only writable members go in. A block's readable members are a superset, and a reader files what
        // it cannot set as meta_information instead, so writing the rest corrupts the block that reads it
        // back: unique_name is the identity of the block the dump was written from, and input_chunk_size,
        // output_chunk_size and stride are constants of a block that is not declared Resampling<>/Stride<>.
        const std::set<std::string>& writable = block->settings().writableMembers();
        property_map                 activeParameters;
        auto                         insertWritable = [&writable, &activeParameters](const property_map& source) {
            for (const auto& [key, value] : source) {
                if (writable.contains(std::string(key))) {
                    activeParameters.insert_or_assign(key, value);
                }
            }
        };
        insertWritable(block->settings().get());
        insertWritable(block->settings().stagedParameters());
        const auto& stored = block->settings().getStoredAll();

        result.emplace(serialization_fields::BLOCK_PARAMETERS, writeParameters(activeParameters));

        using namespace std::string_literals;
        Tensor<pmt::Value> ctxParamsSeq;
        ctxParamsSeq.reserve(stored.size());
        for (const auto& [ctx, ctxParameters] : stored) {
            if (ctx.holds<std::string>()) {
                if (auto str = ctx.value_or(std::string_view{}); str.empty()) {
                    continue;
                }
            }

            for (const auto& [ctxTime, settingsMap] : ctxParameters) {
                pmt::Value::Map ctxParam;

                // Convert ctxTime.context to a string, regardless of its actual type
                std::string contextStr;
                pmt::ValueVisitor([&contextStr]<typename T>(const T& arg) {
                    if constexpr (std::is_same_v<T, std::string>) {
                        contextStr = arg;
                    } else if constexpr (std::is_same_v<std::string_view, T> || std::is_same_v<std::pmr::string, T>) {
                        contextStr = std::string(arg);
                    } else if constexpr (std::is_arithmetic_v<T>) {
                        contextStr = std::to_string(arg);
                    } else {
                        contextStr.clear();
                    }
                }).visit(ctxTime.context);

                ctxParam.emplace(gr::tag::CONTEXT.shortKey(), contextStr);
                ctxParam.emplace(gr::tag::CONTEXT_TIME.shortKey(), ctxTime.time);
                ctxParam.emplace(serialization_fields::BLOCK_PARAMETERS, writeParameters(settingsMap));
                ctxParamsSeq.emplace_back(std::move(ctxParam));
            }
        }
        if (!ctxParamsSeq.empty()) { // absent and empty mean the same thing to a reader, as for meta_information
            result.emplace(serialization_fields::BLOCK_CTX_PARAMETERS, std::move(ctxParamsSeq));
        }
    }

    if (flags & BlockSerializationFlags::Ports) {
        auto serializePortOrCollection = [](const auto& portOrCollection) {
            // TODO: Type names can be mangled. We need proper type names...
            if (auto* port = std::get_if<gr::DynamicPort>(&portOrCollection)) {
                return property_map{
                    {"name", std::string(port->metaInfo.name)}, //
                    {"type", port->typeName()}                  //
                };
            } else {
                auto& coll = std::get<BlockModel::NamedPortCollection>(portOrCollection);
                return property_map{
                    {"name", std::string(coll.name)},                                                    //
                    {"size", static_cast<gr::Size_t>(coll.ports.size())},                                //
                    {"type", coll.ports.empty() ? std::string() : std::string(coll.ports[0].typeName())} //
                };
            }
        };

        property_map inputPorts;
        for (const auto& portOrCollection : block->dynamicInputPorts()) {
            inputPorts[convert_string_domain(BlockModel::portName(portOrCollection))] = serializePortOrCollection(portOrCollection);
        }
        result.emplace(serialization_fields::BLOCK_INPUT_PORTS, std::move(inputPorts));

        property_map outputPorts;
        for (const auto& portOrCollection : block->dynamicOutputPorts()) {
            outputPorts[convert_string_domain(BlockModel::portName(portOrCollection))] = serializePortOrCollection(portOrCollection);
        }
        result.emplace(serialization_fields::BLOCK_OUTPUT_PORTS, std::move(outputPorts));
    }

    return result;
}

property_map serializeBlock(PluginLoader& pluginLoader, const std::shared_ptr<BlockModel>& block, int flags) {
    property_map map;

    if (const gr::Graph* subgraph = block->graph()) {
        map.emplace(serialization_fields::BLOCK_ID, "SUBGRAPH"s);
        map[convert_string_domain(serialization_fields::BLOCK_UNIQUE_NAME)] = std::string(block->uniqueName());

        // a subgraph names itself the way every other block does, so one reader path fits both
        property_map subgraphParameters;
        subgraphParameters[convert_string_domain(serialization_fields::BLOCK_NAME)] = std::string(block->name());
        map[convert_string_domain(serialization_fields::BLOCK_PARAMETERS)]          = std::move(subgraphParameters);

        // and it carries meta_information the way every other block does
        if (!block->metaInformation().empty()) {
            map.emplace(serialization_fields::BLOCK_META_INFORMATION, block->metaInformation());
        }

        {
            property_map subgraphMap;

            if (flags & BlockSerializationFlags::Children) {
                subgraphMap = detail::saveGraphToMap(pluginLoader, *subgraph);
            }

            // one entry per exported port: {inner block, direction, internal port name, exported port name}
            Tensor<pmt::Value> exportedPortsData;
            auto               writeExportedPorts = [&exportedPortsData](const property_map& portsPerBlock, const std::string& direction) {
                for (const auto& [blockUniqueName, portsValue] : portsPerBlock) {
                    const auto ports = checked_access_ptr<const property_map, false>{portsValue.get_if<property_map>()};
                    if (ports == nullptr) {
                        continue;
                    }
                    for (const auto& [internalName, mapping] : *ports) {
                        std::string exportedName(internalName);
                        if (const auto details = checked_access_ptr<const property_map, false>{mapping.get_if<property_map>()}; details != nullptr) {
                            if (const auto it = details->find("exportedName"); it != details->cend()) {
                                exportedName = std::string(it->second.value_or(std::string_view{}));
                            }
                        }
                        exportedPortsData.push_back(Tensor<pmt::Value>(data_from, {gr::pmt::Value(std::string(blockUniqueName)), gr::pmt::Value(direction), gr::pmt::Value(std::string(internalName)), gr::pmt::Value(exportedName)}));
                    }
                }
            };
            writeExportedPorts(block->exportedInputPorts(), "INPUT"s);
            writeExportedPorts(block->exportedOutputPorts(), "OUTPUT"s);

            subgraphMap["exported_ports"] = std::move(exportedPortsData);
            map["graph"]                  = std::move(subgraphMap);
        }

        if (const auto* schedulerModel = dynamic_cast<const SchedulerModel*>(block.get()); schedulerModel != nullptr) {
            property_map schedulerMap;
            schedulerMap["id"] = pluginLoader.schedulerRegistry().typeName(block);
            map["scheduler"]   = std::move(schedulerMap);
        }

    } else {
        map = serializeBlockImpl(pluginLoader, block, flags);
    }

    return map;
}
} // namespace gr
