#ifndef GNURADIO_RECIPE_INSTANTIATION_HPP
#define GNURADIO_RECIPE_INSTANTIATION_HPP

#include <expected>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gnuradio-4.0/BlockModel.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/RecipeParameters.hpp>
#include <gnuradio-4.0/Tensor.hpp>

namespace gr::detail {

/// the exported_parameters list of a definition's SUBGRAPH entry, as declarations
inline std::expected<std::vector<recipe::ParameterDeclaration>, gr::Error> readRecipeDeclarations(const property_map& blockEntry) {
    std::vector<recipe::ParameterDeclaration> declarations;
    const auto                                listIt = blockEntry.find("exported_parameters");
    if (listIt == blockEntry.end()) {
        return declarations;
    }
    const auto* list = listIt->second.get_if<Tensor<pmt::Value>>();
    if (list == nullptr) {
        return std::unexpected(gr::Error("recipe_expression_parse: exported_parameters is not a list"));
    }
    for (const auto& entryValue : *list) {
        const auto* entry = entryValue.get_if<property_map>();
        if (entry == nullptr) {
            return std::unexpected(gr::Error("recipe_expression_parse: an exported_parameters entry is not a map"));
        }
        recipe::ParameterDeclaration declaration;
        for (const auto& [key, value] : *entry) {
            const std::string_view keyView(key);
            const std::string_view valueView = value.value_or(std::string_view{});
            if (keyView == "name" && !valueView.empty()) {
                declaration.name.assign(valueView);
            } else if (keyView == "type" && !valueView.empty()) {
                declaration.type.assign(valueView);
            } else if (keyView == "default") {
                declaration.defaultValue = value;
            } else if (keyView == "doc" && !valueView.empty()) {
                declaration.doc.assign(valueView);
            }
        }
        if (declaration.name.empty() || declaration.type.empty()) {
            return std::unexpected(gr::Error("recipe_expression_parse: an exported parameter needs a name and a type"));
        }
        declarations.push_back(std::move(declaration));
    }
    return declarations;
}

/// the parameters a definition exports: the declarations on the composite entry it is built
/// around, and none for a definition that carries no composite or declares nothing
inline std::expected<std::vector<recipe::ParameterDeclaration>, gr::Error> recipeDeclarationsOf(const property_map& definition) {
    const auto blocksIt = definition.find("blocks");
    if (blocksIt == definition.end()) {
        return std::vector<recipe::ParameterDeclaration>{};
    }
    const auto* blocksList = blocksIt->second.get_if<Tensor<pmt::Value>>();
    if (blocksList == nullptr) {
        return std::vector<recipe::ParameterDeclaration>{};
    }
    for (const auto& entry : *blocksList) {
        if (const auto* blockEntry = entry.get_if<property_map>(); blockEntry != nullptr && blockEntry->contains("graph")) {
            return readRecipeDeclarations(*blockEntry);
        }
    }
    return std::vector<recipe::ParameterDeclaration>{};
}

/**
 * @brief A block's parameters, split the way the type that receives them takes them.
 *
 * A block built from a YAML definition takes only the parameters the definition exports, since its interior is
 * derived from them and their values are needed before the interior exists. It refuses any other key by name, the
 * `name` every caller supplies included. `exported` goes with the instantiation, and `remaining` loads as settings.
 * For any other type `exported` is empty.
 *
 * `fromDefinition` tells the two apart where an empty `exported` cannot: a definition that declares nothing still
 * refuses an undeclared parameter, so a caller hands the whole map at construction only to a registered block.
 */
struct RecipeParameterSplit {
    bool         fromDefinition = false;
    property_map exported;
    property_map remaining;
};

/// splits `parameters` for `blockType` as read from `loader`'s definitions; a definition whose
/// declarations cannot be read is left to the instantiation to refuse, which is where the reason
/// is reported
inline RecipeParameterSplit splitRecipeParameters(PluginLoader& loader, std::string_view blockType, property_map parameters) {
    RecipeParameterSplit split;

    const auto& definitions = loader.definitionForBlockName();
    const auto  definition  = definitions.find(std::string(blockType));
    if (definition == definitions.end()) {
        split.remaining = std::move(parameters);
        return split;
    }
    split.fromDefinition = true;

    if (const auto declarations = recipeDeclarationsOf(definition->second.definition); declarations.has_value()) {
        for (const auto& declaration : *declarations) {
            if (const auto it = parameters.find(convert_string_domain(declaration.name)); it != parameters.end()) {
                split.exported.emplace(it->first, it->second);
                parameters.erase(it);
            }
        }
    }
    split.remaining = std::move(parameters);
    return split;
}

/// loads a split's `remaining` keys into `block`'s settings; a refusal names the block, its type and the refused key
inline std::expected<void, gr::Error> loadRemainingSettings(BlockModel& block, std::string_view blockName, std::string_view blockType, const property_map& remaining) {
    try {
        block.settings().loadParametersFromPropertyMap(remaining);
    } catch (const gr::exception& refusal) {
        return std::unexpected(gr::Error(std::format("Unable to create block '{}' of type '{}': {}", blockName, blockType, refusal.message), refusal.sourceLocation));
    }
    return {};
}

/// builds a block of a definition-backed type from `split.exported`, labels it `blockName` when that is not empty and
/// loads `split.remaining` as its settings. A refusal names the block, its type and the reason. The value is null when
/// the loader builds nothing for `blockType`.
inline std::expected<std::shared_ptr<BlockModel>, gr::Error> instantiateRecipe(PluginLoader& loader, std::string_view blockType, std::string_view blockName, const RecipeParameterSplit& split) {
    std::expected<std::shared_ptr<BlockModel>, gr::Error> instantiated = loader.instantiateOrError(blockType, split.exported);
    if (!instantiated.has_value()) {
        return std::unexpected(gr::Error(std::format("Unable to create block '{}' of type '{}': {}", blockName, blockType, instantiated.error().message)));
    }
    if (*instantiated == nullptr) {
        return instantiated;
    }
    if (!blockName.empty()) {
        (*instantiated)->setName(std::string(blockName));
    }
    if (auto loaded = loadRemainingSettings(**instantiated, blockName, blockType, split.remaining); !loaded.has_value()) {
        return std::unexpected(loaded.error());
    }
    return instantiated;
}

/// the `name` among a split's remaining keys, empty when there is none
[[nodiscard]] inline std::string_view remainingName(const RecipeParameterSplit& split) {
    const auto nameIt = split.remaining.find("name");
    return nameIt == split.remaining.end() ? std::string_view{} : nameIt->second.value_or(std::string_view{});
}

} // namespace gr::detail

#endif // GNURADIO_RECIPE_INSTANTIATION_HPP
