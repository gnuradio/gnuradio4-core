#ifndef GNURADIO_BLOCK_REGISTRY_HPP
#define GNURADIO_BLOCK_REGISTRY_HPP

#include <gnuradio-4.0/meta/simd.hpp>

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

namespace gr {

using namespace std::string_literals;
using namespace std::string_view_literals;

using BlockFactory = std::unique_ptr<BlockModel> (*)(property_map);

/**
 * @brief What a generated definition unit exports beside its factory.
 *
 * `name` and `alias` are the strings the typed `insert<TBlock>()` path would have derived for the
 * same block, so a declaration-only registration unit can hand them to `insertBlockFactory()`
 * without naming the type. `version` and `status` are read in `makeBlockRegistration<TBlock>()`,
 * where the type is still named, so the unit that carries them names none either.
 */
struct BlockRegistration {
    std::string    name;
    std::string    alias;
    BlockFactory   factory = nullptr;
    block::Version version = block::kDefaultVersion;
    block::Status  status{};
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

    /// one registered revision of one key: its factory and what the type declares
    struct TVersionHandler {
        decltype(this_t::factoryProto)* createFunction = nullptr;
        block::Status                   status{};
    };

    /// A key holds every version registered under it, ordered, so the newest is the last entry. Two
    /// registrations of one key and one version collide, the last winning.
    struct TTypeHandler {
        std::string                               alias;
        std::map<block::Version, TVersionHandler> versions;
    };

    std::map<std::string, TTypeHandler, std::less<>> _blockTypeHandlers;
    std::size_t                                      _generation = 0UZ;

    [[nodiscard]] const TTypeHandler* handlerFor(std::string_view blockName) const {
        const auto it = _blockTypeHandlers.find(blockName);
        return it == _blockTypeHandlers.cend() ? nullptr : std::addressof(it->second);
    }

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
    /// Reports whether it added anything: a key, or a version under an existing key.
    bool insert(std::string_view name, std::string_view alias, decltype(this_t::factoryProto)* factory, block::Version version = block::kDefaultVersion, block::Status status = {}) {
        const TVersionHandler entry{.createFunction = factory, .status = status};

        auto addVersion = [&entry, version](TTypeHandler& handler) { return handler.versions.insert_or_assign(version, entry).second; };

        auto [nameIt, nameInserted] = _blockTypeHandlers.try_emplace(std::string(name));
        const bool nameAdded        = addVersion(nameIt->second) || nameInserted;
        nameIt->second.alias        = std::string(alias); // the last registration of a key names its alias
        ++_generation;

        bool aliasAdded = false;
        if (!alias.empty()) {
            // the alias entry carries no alias of its own: typeName() reads the alias off the key it was asked about
            auto [aliasIt, aliasInserted] = _blockTypeHandlers.try_emplace(std::string(alias));
            aliasAdded                    = addVersion(aliasIt->second) || aliasInserted;
            ++_generation;
        }

        return nameAdded || aliasAdded;
    }

    template<BlockLike TBlock>
    requires std::is_constructible_v<TBlock, property_map>
    bool insert(std::string_view alias = "", std::string_view aliasParameters = "") {
        return insert(gr::meta::type_name<TBlock>(), makeRegistryAlias(alias, aliasParameters), defaultFactory<TBlock>, block::versionOf<TBlock>(), block::statusOf<TBlock>());
    }
#else
    bool insert([[maybe_unused]] std::string_view name, [[maybe_unused]] std::string_view alias, [[maybe_unused]] decltype(this_t::factoryProto)* factory, [[maybe_unused]] block::Version version = block::kDefaultVersion, [[maybe_unused]] block::Status status = {}) { return false; }

    template<BlockLike TBlock>
    requires std::is_constructible_v<TBlock, property_map>
    bool insert([[maybe_unused]] std::string_view alias = "", [[maybe_unused]] std::string_view aliasParameters = "") {
        return false;
        // disables plugin system in favour of faster compile-times and when runtime or Python wrapping APIs are not requrired
        // e.g. for compile-time only flow-graphs or for CI runners
    }
#endif

    /// the newest version registered under `blockName`
    [[nodiscard]] std::unique_ptr<TModel> create(std::string_view blockName, property_map blockParams) const {
        const TTypeHandler* handler = handlerFor(blockName);
        if (handler == nullptr || handler->versions.empty()) {
            return nullptr;
        }
        return std::prev(handler->versions.cend())->second.createFunction(std::move(blockParams));
    }

    /// the one version named, or nothing; the instance records that it was pinned
    [[nodiscard]] std::unique_ptr<TModel> create(std::string_view blockName, block::Version version, property_map blockParams) const {
        const TTypeHandler* handler = handlerFor(blockName);
        if (handler == nullptr) {
            return nullptr;
        }
        const auto versionIt = handler->versions.find(version);
        if (versionIt == handler->versions.cend()) {
            return nullptr;
        }
        auto created = versionIt->second.createFunction(std::move(blockParams));
        // only a block model records the pin; a scheduler model has no such state
        if constexpr (requires { created->setPinnedVersion(version); }) {
            if (created) {
                created->setPinnedVersion(version);
            }
        }
        return created;
    }

    /// every version registered under `blockName`, oldest first; empty for a key that is not registered
    [[nodiscard]] std::vector<block::Version> versions(std::string_view blockName) const {
        const TTypeHandler* handler = handlerFor(blockName);
        if (handler == nullptr) {
            return {};
        }
        auto view = handler->versions | std::views::keys;
        return {view.begin(), view.end()};
    }

    [[nodiscard]] std::optional<block::Version> newestVersion(std::string_view blockName) const {
        const TTypeHandler* handler = handlerFor(blockName);
        if (handler == nullptr || handler->versions.empty()) {
            return std::nullopt;
        }
        return std::prev(handler->versions.cend())->first;
    }

    /// what that one registration declares, or nothing when the key or the version is not registered
    [[nodiscard]] std::optional<block::Status> status(std::string_view blockName, block::Version version) const {
        const TTypeHandler* handler = handlerFor(blockName);
        if (handler == nullptr) {
            return std::nullopt;
        }
        const auto versionIt = handler->versions.find(version);
        if (versionIt == handler->versions.cend()) {
            return std::nullopt;
        }
        return versionIt->second.status;
    }

    [[nodiscard]] std::vector<std::string> keys() const {
        auto view = _blockTypeHandlers | std::views::keys;
        return {view.begin(), view.end()};
    }

    /// increments once per registered key, whether the key is new or replaces one already held
    [[nodiscard]] std::size_t generation() const noexcept { return _generation; }

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
    constexpr auto           name     = refl::class_name<TBlock>;
    constexpr auto           longname = refl::type_name<TBlock>;
    constexpr block::Version version  = block::versionOf<TBlock>();
    constexpr block::Status  status   = block::statusOf<TBlock>();
    if constexpr (OverrideName != "") {
        return {gr::meta::type_name<TBlock>(), makeRegistryAlias(OverrideName, {}), factory, version, status};
    } else if constexpr (name != longname) {
        constexpr auto tmpl = longname.substring(name.size + 1_cw, longname.size - 2_cw - name.size);
        return {gr::meta::type_name<TBlock>(), makeRegistryAlias(name, tmpl), factory, version, status};
    } else {
        return {gr::meta::type_name<TBlock>(), makeRegistryAlias(name, {}), factory, version, status};
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
