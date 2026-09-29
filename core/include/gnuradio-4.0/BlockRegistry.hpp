#ifndef GNURADIO_BLOCK_REGISTRY_HPP
#define GNURADIO_BLOCK_REGISTRY_HPP

#include <gnuradio-4.0/meta/simd.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
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

// The version of the plugin interface, raised by every change to the layout of a type that crosses the plugin
// boundary: `gr_plugin_base` itself, and the `BlockModel` and `SchedulerModel` interfaces whose objects a plugin or a
// registry factory hands back. A plugin records the version it was compiled against. A registry entry records the
// version of the code that calls `insert()`, so a block entry added through `insertBlockFactory()` records the
// version of core. A host loads only a plugin whose version equals its own. Of the entries a shared object registers,
// the host reads the versions of the schedulers and keeps those only at its own version, because a virtual call
// through a mismatched interface reaches the wrong function.
#define GR_PLUGIN_CURRENT_ABI_VERSION 4

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

    // a version belongs to the factory it was recorded with, so an entry a later insertion replaced without recording
    // one reads as unversioned
    struct RecordedAbiVersion {
        decltype(this_t::factoryProto)* createFunction = nullptr;
        std::uint8_t                    abiVersion     = 0;
    };

    // `_blockTypeHandlers` and `_generation` keep the layout of a registry that records no version: a shared object
    // built against one inserts into them through its own inline copy of `insert()`, and never into `_abiVersions`.
    // The reverse does not hold: during its static initialization, a shared object built against this layout writes
    // `_abiVersions` past the end of an older registry object, and a host built against that layout cannot load it
    // safely.
    std::map<std::string, TTypeHandler, std::less<>>       _blockTypeHandlers;
    std::size_t                                            _generation = 0UZ;
    std::map<std::string, RecordedAbiVersion, std::less<>> _abiVersions;

public:
    /// every entry of a registry, as `takeEntries()` moves them out and `restoreEntries()` puts them back
    struct Entries {
        std::map<std::string, TTypeHandler, std::less<>>       handlers;
        std::map<std::string, RecordedAbiVersion, std::less<>> abiVersions;
    };

    GeneralRegistry()                               = default;
    GeneralRegistry(const this_t& other)            = delete;
    GeneralRegistry& operator=(const this_t& other) = delete;

    GeneralRegistry(this_t&& other) noexcept : _blockTypeHandlers(std::exchange(other._blockTypeHandlers, {})), _abiVersions(std::exchange(other._abiVersions, {})) {}
    GeneralRegistry& operator=(this_t&& other) noexcept {
        auto tmp = std::move(other);
        std::swap(_blockTypeHandlers, tmp._blockTypeHandlers);
        std::swap(_abiVersions, tmp._abiVersions);
        return *this;
    }
    ~GeneralRegistry() = default;

#ifdef GR_ENABLE_BLOCK_REGISTRY
    /// Adds an entry a generated definition unit already produced: nothing here names the block type. The entry
    /// records `abiVersion`, whose default argument is evaluated where the call is written: the version recorded is the
    /// `GR_PLUGIN_CURRENT_ABI_VERSION` of the translation unit that registers, whichever copy of this function runs.
    bool insert(std::string_view name, std::string_view alias, decltype(this_t::factoryProto)* factory, std::uint8_t abiVersion = GR_PLUGIN_CURRENT_ABI_VERSION) {
        auto handler = TTypeHandler{.alias = std::string(alias), .createFunction = factory};

        auto resName = _blockTypeHandlers.insert_or_assign(std::string(name), handler);
        _abiVersions.insert_or_assign(std::string(name), RecordedAbiVersion{.createFunction = factory, .abiVersion = abiVersion});
        ++_generation;

        bool aliasInserted = false;
        if (!alias.empty()) {
            handler.alias.clear();
            auto resAlias = _blockTypeHandlers.insert_or_assign(std::string(alias), handler);
            _abiVersions.insert_or_assign(std::string(alias), RecordedAbiVersion{.createFunction = factory, .abiVersion = abiVersion});
            aliasInserted = resAlias.second;
            ++_generation;
        }

        return resName.second || aliasInserted;
    }

    template<BlockLike TBlock>
    requires std::is_constructible_v<TBlock, property_map>
    bool insert(std::string_view alias = "", std::string_view aliasParameters = "", std::uint8_t abiVersion = GR_PLUGIN_CURRENT_ABI_VERSION) {
        return insert(gr::meta::type_name<TBlock>(), makeRegistryAlias(alias, aliasParameters), defaultFactory<TBlock>, abiVersion);
    }
#else
    bool insert([[maybe_unused]] std::string_view name, [[maybe_unused]] std::string_view alias, [[maybe_unused]] decltype(this_t::factoryProto)* factory, [[maybe_unused]] std::uint8_t abiVersion = GR_PLUGIN_CURRENT_ABI_VERSION) { return false; }

    template<BlockLike TBlock>
    requires std::is_constructible_v<TBlock, property_map>
    bool insert([[maybe_unused]] std::string_view alias = "", [[maybe_unused]] std::string_view aliasParameters = "", [[maybe_unused]] std::uint8_t abiVersion = GR_PLUGIN_CURRENT_ABI_VERSION) {
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

    /// increments once per registered key, whether the key is new or replaces one already held
    [[nodiscard]] std::size_t generation() const noexcept { return _generation; }

    [[nodiscard]] bool contains(std::string_view blockName) const { return _blockTypeHandlers.contains(blockName); }

    /// the plugin ABI version the entry under `key` was registered at; empty for a key the registry does not hold and
    /// for an entry whose registration recorded none, as code built against a registry without versions registers
    [[nodiscard]] std::optional<std::uint8_t> abiVersion(std::string_view key) const {
        const auto handler  = _blockTypeHandlers.find(key);
        const auto recorded = _abiVersions.find(key);
        if (handler == _blockTypeHandlers.end() || recorded == _abiVersions.end() || recorded->second.createFunction != handler->second.createFunction) {
            return std::nullopt;
        }
        return recorded->second.abiVersion;
    }

    /// Moves every entry out and leaves the generation as it is, so that the registry holds only what is registered
    /// afterwards until `restoreEntries()` puts the taken entries back.
    [[nodiscard]] Entries takeEntries() { return {.handlers = std::exchange(_blockTypeHandlers, {}), .abiVersions = std::exchange(_abiVersions, {})}; }

    /// Puts back the entries `takeEntries()` returned. With `keepRegistered`, an entry registered since then stays and
    /// replaces a taken one under the same key; without it, every such entry is dropped.
    void restoreEntries(Entries taken, bool keepRegistered) {
        if (keepRegistered) {
            for (auto& [key, handler] : _blockTypeHandlers) {
                taken.handlers.insert_or_assign(key, std::move(handler));
            }
            for (const auto& [key, recorded] : _abiVersions) {
                taken.abiVersions.insert_or_assign(key, recorded);
            }
        }
        _blockTypeHandlers = std::move(taken.handlers);
        _abiVersions       = std::move(taken.abiVersions);
    }

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
        _abiVersions.insert(anotherRegistry._abiVersions.cbegin(), anotherRegistry._abiVersions.cend());
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
