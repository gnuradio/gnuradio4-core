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

namespace {

/// Writes the block's `parameters`, its name and the keys a caller gave it, and its `tag_couplings` when it couples any setting to
/// a tag key. A key waiting to apply goes in at its waiting value, so the save commits nothing; a value the block
/// derived, a tag set or a device reported stays out, since a load would make it given.
void serializeSettings(const std::shared_ptr<BlockModel>& block, property_map& result) {
    pmt::Value::Map parameters;
    for (const auto& [key, value] : settings::detail::FrameworkAccess::savedValues(settings::detail::settingsOf(*block))) {
        parameters[key] = value;
    }
    // a graph file names every block, given or not; a name the settings hold is saved, also one waiting for the start
    if (const auto nameKey = convert_string_domain(serialization_fields::BLOCK_NAME); !parameters.contains(nameKey)) {
        parameters[nameKey] = std::string(block->name());
    }
    result.emplace(serialization_fields::BLOCK_PARAMETERS, std::move(parameters));
    if (property_map couplings = block->tagCouplings(); !couplings.empty()) {
        result.emplace(serialization_fields::BLOCK_TAG_COUPLINGS, std::move(couplings));
    }
}

} // namespace

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
        serializeSettings(block, result);
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

    // a recipe composite saves as its recipe's type with its parameters, and a load builds its inner graph again
    if (const std::string_view recipeType = block->recipeType(); !recipeType.empty()) {
        map                                                        = serializeBlockImpl(pluginLoader, block, flags);
        map[convert_string_domain(serialization_fields::BLOCK_ID)] = std::string(recipeType);
    } else if (const gr::Graph* subgraph = block->graph(); subgraph != nullptr) {
        map.emplace(serialization_fields::BLOCK_ID, "SUBGRAPH"s);
        map[convert_string_domain(serialization_fields::BLOCK_UNIQUE_NAME)] = std::string(block->uniqueName());

        // A subgraph writes its settings and its name as a block does.
        if (flags & BlockSerializationFlags::Settings) {
            serializeSettings(block, map);
        }
        pmt::Value& parameters = map[convert_string_domain(serialization_fields::BLOCK_PARAMETERS)];
        if (parameters.get_if<property_map>() == nullptr) {
            parameters = property_map{};
        }
        // a subgraph that loads again holds the inner graph as saved, and no recipe parameter
        for (const std::string& parameter : settings::detail::FrameworkAccess::parameterNames(settings::detail::settingsOf(*block))) {
            parameters.get_if<property_map>()->erase(convert_string_domain(parameter));
            if (const auto couplings = map.find(convert_string_domain(serialization_fields::BLOCK_TAG_COUPLINGS)); couplings != map.end()) {
                if (property_map* coupled = couplings->second.get_if<property_map>(); coupled != nullptr) {
                    coupled->erase(convert_string_domain(parameter));
                }
            }
        }
        // a name the settings hold may wait for a nested scheduler's next start, and name() returns the old one until then
        if (const auto nameKey = convert_string_domain(serialization_fields::BLOCK_NAME); !parameters.get_if<property_map>()->contains(nameKey)) {
            (*parameters.get_if<property_map>())[nameKey] = std::string(block->name());
        }

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
