#ifndef GNURADIO_BLOCK_REGISTRY_HPP
#define GNURADIO_BLOCK_REGISTRY_HPP

#include <memory>
#include <string>
#include <string_view>

#include <gnuradio-4.0/config.hpp>
#include <gnuradio-4.0/meta/utils.hpp>

#include <gnuradio-4.0/BlockModel.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>

#include <gnuradio-4.0/BlockRegistration.hpp>
#include <gnuradio-4.0/Export.hpp>

/**
 *  namespace gr {
 *  template<typename T> struct AlgoImpl1 {};
 *  template<typename T> struct AlgoImpl2 {};
 *
 *  // register block with arbitrary NTTPs (here: 3UZ) and expand T in [float,double], U in [short, int, long, long long]
 *  GR_REGISTER_BLOCK(gr::basic::BlockN, ([T], [U], 3UZ), [ float, double ], [ short, int, long, long long ])
 *  // register block with arbitrary NTTPs (here: 4UZ) and expand T for [short], U for [short] only
 *  GR_REGISTER_BLOCK("CustomBlockNameN", gr::basic::BlockN, ([T], [U], 4UZ, gr::basic::AlgoImpl2<[T]>), [ short ], [ short ])
 *
 *  template<typename T, typename U, std::size_t N, typename Alog = AlgoImpl1<T>>
 *  struct BlockN : public gr::IBlock { ... };
 *
 *  } // namespace gr::basic
 *
 * other macro variants options:
 * GR_REGISTER_BLOCK("MyBlockName", gr::basic::Block1, ([T], [U]), [ float, double ], [int])
 * GR_REGISTER_BLOCK(gr::basic::Block0)
 * GR_REGISTER_BLOCK("blockN.hpp", gr::basic::BlockN, ([T],[U],3UZ,SomeAlgo<[T]>), [ short, int], [double])
 */
#define GR_REGISTER_BLOCK(...) /* Marker macro for parse_registrations */

namespace gr {

using namespace std::string_literals;
using namespace std::string_view_literals;

using BlockFactory = std::unique_ptr<BlockModel> (*)(property_map);

/**
 * @brief What a generated definition unit exports beside its factory.
 *
 * `name` and `alias` are the strings the typed `insert<TBlock>()` path would have derived for the
 * same block, so a declaration-only registration unit can hand them to `insertBlockFactory()`
 * without naming the type.
 */
struct BlockRegistration {
    std::string  name;
    std::string  alias;
    BlockFactory factory = nullptr;
};

/// The registry alias for a block registered as `alias` with template parameters `aliasParameters`.
[[nodiscard]] inline std::string makeRegistryAlias(std::string_view alias, std::string_view aliasParameters) {
    if (alias.empty()) {
        return std::string{};
    }
    if (alias[0] == '=') {
        return std::string(alias.substr(1));
    }
    if (aliasParameters.empty()) {
        return meta::detail::makePortableTypeName(alias);
    }
    return meta::detail::makePortableTypeName(std::string{alias} + "<" + std::string{aliasParameters} + ">");
}

template<typename TModel, template<typename...> typename TWrapper>
class GeneralRegistry {
    using this_t = GeneralRegistry<TModel, TWrapper>;
    static std::unique_ptr<TModel> factoryProto(property_map params);

    template<typename TBlock>
    static std::unique_ptr<TModel> defaultFactory(property_map params) { //
        return std::make_unique<TWrapper<TBlock>>(std::move(params));
    }

    struct TTypeHandler {
        std::string                     alias;
        decltype(this_t::factoryProto)* createFunction = nullptr;
    };

    std::map<std::string, TTypeHandler, std::less<>> _blockTypeHandlers;

public:
    GeneralRegistry()                               = default;
    GeneralRegistry(const this_t& other)            = delete;
    GeneralRegistry& operator=(const this_t& other) = delete;

    GeneralRegistry(this_t&& other) noexcept : _blockTypeHandlers(std::exchange(other._blockTypeHandlers, {})) {}
    GeneralRegistry& operator=(this_t&& other) noexcept {
        auto tmp = std::move(other);
        std::swap(_blockTypeHandlers, tmp._blockTypeHandlers);
        return *this;
    }
    ~GeneralRegistry() = default;

#ifdef GR_ENABLE_BLOCK_REGISTRY
    /// Adds an entry a generated definition unit already produced: nothing here names the block type.
    bool insert(std::string_view name, std::string_view alias, decltype(this_t::factoryProto)* factory) {
        auto handler = TTypeHandler{.alias = std::string(alias), .createFunction = factory};

        auto resName = _blockTypeHandlers.insert_or_assign(std::string(name), handler);

        bool aliasInserted = false;
        if (!alias.empty()) {
            handler.alias.clear();
            auto resAlias = _blockTypeHandlers.insert_or_assign(std::string(alias), handler);
            aliasInserted = resAlias.second;
        }

        return resName.second || aliasInserted;
    }

    template<BlockLike TBlock>
    requires std::is_constructible_v<TBlock, property_map>
    bool insert(std::string_view alias = "", std::string_view aliasParameters = "") {
        return insert(gr::meta::type_name<TBlock>(), makeRegistryAlias(alias, aliasParameters), defaultFactory<TBlock>);
    }
#else
    bool insert([[maybe_unused]] std::string_view name, [[maybe_unused]] std::string_view alias, [[maybe_unused]] decltype(this_t::factoryProto)* factory) { return false; }

    template<BlockLike TBlock>
    requires std::is_constructible_v<TBlock, property_map>
    bool insert([[maybe_unused]] std::string_view alias = "", [[maybe_unused]] std::string_view aliasParameters = "") {
        return false;
        // disables plugin system in favour of faster compile-times and when runtime or Python wrapping APIs are not requrired
        // e.g. for compile-time only flow-graphs or for CI runners
    }
#endif

    [[nodiscard]] std::unique_ptr<TModel> create(std::string_view blockName, property_map blockParams) const {
        if (auto blockIt = _blockTypeHandlers.find(blockName); blockIt != _blockTypeHandlers.end()) {
            return blockIt->second.createFunction(std::move(blockParams));
        }

        return nullptr;
    }

    [[nodiscard]] std::vector<std::string> keys() const {
        auto view = _blockTypeHandlers | std::views::keys;
        return {view.begin(), view.end()};
    }

    [[nodiscard]] bool contains(std::string_view blockName) const { return _blockTypeHandlers.contains(blockName); }

    std::string typeName(const std::shared_ptr<BlockModel>& block) {
        auto name = block->typeName();
        auto it   = _blockTypeHandlers.find(name);
        if (it != _blockTypeHandlers.end() && !it->second.alias.empty()) {
            return it->second.alias;
        }
        return std::string(name);
    }

    void merge(this_t& anotherRegistry) {
        if (this == std::addressof(anotherRegistry)) {
            return;
        }

        _blockTypeHandlers.insert(anotherRegistry._blockTypeHandlers.cbegin(), anotherRegistry._blockTypeHandlers.cend());
    }
};

class BlockRegistry : public GeneralRegistry<BlockModel, BlockWrapper> {
    friend BlockRegistry& globalBlockRegistry(std::source_location location);
};

class SchedulerRegistry : public GeneralRegistry<SchedulerModel, SchedulerWrapper> {
    friend SchedulerRegistry& globalSchedulerRegistry(std::source_location location);
};

/**
 * @brief The entry `registerBlock<TBlock, OverrideName>()` would produce, for a factory the caller
 * owns.
 *
 * This is what a generated definition unit exports: it derives the key and the alias through the
 * same `meta::type_name<TBlock>()` and `makeRegistryAlias()` the `insert<TBlock>()` path uses, so a
 * registration through `insertBlockFactory()` is indistinguishable from the typed one.
 */
template<typename TBlock, meta::fixed_string OverrideName = "">
[[nodiscard]] BlockRegistration makeBlockRegistration(BlockFactory factory) {
    using namespace vir::literals;
    constexpr auto name     = refl::class_name<TBlock>;
    constexpr auto longname = refl::type_name<TBlock>;
    if constexpr (OverrideName != "") {
        return {gr::meta::type_name<TBlock>(), makeRegistryAlias(OverrideName, {}), factory};
    } else if constexpr (name != longname) {
        constexpr auto tmpl = longname.substring(name.size + 1_cw, longname.size - 2_cw - name.size);
        return {gr::meta::type_name<TBlock>(), makeRegistryAlias(name, tmpl), factory};
    } else {
        return {gr::meta::type_name<TBlock>(), makeRegistryAlias(name, {}), factory};
    }
}

GNURADIO_EXPORT
SchedulerRegistry& globalSchedulerRegistry(std::source_location location = std::source_location::current());

} // namespace gr

extern "C" {
GNURADIO_EXPORT
gr::BlockRegistry* grGlobalBlockRegistry(std::source_location location = std::source_location::current());

GNURADIO_EXPORT
gr::SchedulerRegistry* grGlobalSchedulerRegistry(std::source_location location = std::source_location::current());
}

#endif // GNURADIO_BLOCK_REGISTRY_HPP
