#ifndef GNURADIO_COMPOSITE_SETTINGS_HPP
#define GNURADIO_COMPOSITE_SETTINGS_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/RecipeParameters.hpp>
#include <gnuradio-4.0/Settings.hpp>

namespace gr::settings::detail {

// an inner block where an input of the composite enters, with the inner blocks a stream from that input reaches
struct StreamEntry {
    SettingsBase*              entry = nullptr;
    std::vector<SettingsBase*> reached;
};

// the prefix of every inner tag key of the composite of that unique name
[[nodiscard]] std::string innerTagKeyPrefix(std::string_view compositeName);

// the inner block a binding's name path names: its settings and its name
struct InnerTarget {
    SettingsBase* settings = nullptr;
    std::string   blockName;
};

// finds the inner block of a binding's name path in the composite's graph as it stands, or says why it cannot
using InnerResolver = std::function<std::expected<InnerTarget, std::string>(std::span<const std::string> namePath)>;

// the settings one inner block receives from a call, and the parameters of the call behind each of its keys
struct InnerChanges {
    SettingsBase*                                                settings = nullptr;
    std::string                                                  blockName;
    property_map                                                 changes;
    std::map<std::string, std::vector<std::string>, std::less<>> parametersOf;
};

/**
 * @brief The parameters of a recipe composite: their declarations, the inner settings each derives, the values as
 * stored, and the record of the values a call gave.
 *
 * A call on the composite's settings that names a parameter passes five steps. Each parameter converts to its declared
 * type; each binding that reads a parameter of the call evaluates over the stored parameters with the call's in place;
 * each inner block checks the settings it receives. The own keys merge, each inner block merges its settings, and the
 * parameters are stored. The composite's lock is held from the first step to the last, and before any block's lock. A
 * refusal before the merges changes nothing. The composite's own keys apply at the call, since no work call reaches a
 * composite.
 *
 * A set with `noWait` or from a scheduler's worker thread holds the composite lock only to derive the inner keys, store
 * the parameters and pass each inner block its keys as a no-wait set, whose own queue keeps the latest value while that
 * block is held; an inner refusal is that block's failed notification, and the parameter stays stored. A tag tries each
 * inner block's lock, and a block that another thread holds takes its keys into its queue. While the composite lock is
 * held, such a set or tag leaves the latest value of each parameter; the next set, check or tag that lets the lock go
 * passes them on.
 */
struct CompositeSettings {
    using Clock = std::chrono::steady_clock;

    CompositeSettings(std::vector<recipe::ParameterDeclaration> declarations, std::vector<recipe::Binding> bindings, std::vector<pmt::Value> values, const property_map& given, InnerResolver resolve);
    CompositeSettings(const CompositeSettings&)            = delete;
    CompositeSettings& operator=(const CompositeSettings&) = delete;

    [[nodiscard]] bool                     declares(std::string_view name) const noexcept;
    [[nodiscard]] bool                     namesParameter(const property_map& changes) const noexcept;
    [[nodiscard]] std::vector<std::string> parameterNames() const;
    [[nodiscard]] bool                     isGiven(std::string_view name) const;
    [[nodiscard]] property_map             record() const;                                         // each parameter a call gave, at the value it gave
    void                                   couple(std::string_view parameter, std::string tagKey); // an empty key removes the coupling
    [[nodiscard]] property_map             couplings() const;                                      // each coupled parameter's tag key

    // a call that names a parameter, made at `calledAt`: a set that waits takes each block lock and waits for each inner
    // block as `set` does, and is refused when a block lock stays held to its deadline at the check; a lock that times
    // out at the commit fails the parameters behind its block. A set that does not wait passes the inner keys on as
    // no-wait sets. A check stores nothing, and takes each block lock up to `deadline`, none for no bound
    [[nodiscard]] std::expected<Values, Rejected> set(SettingsBase& own, const property_map& changes, Clock::time_point calledAt, Timeout timeout);
    [[nodiscard]] std::expected<void, Rejected>   check(SettingsBase& own, const property_map& changes, std::optional<Clock::time_point> deadline, Clock::time_point calledAt);
    // each parameter asked, or every parameter when none is asked, at its value as stored; the rest of the keys asked
    [[nodiscard]] Values readParameters(std::span<const std::string> keys, std::vector<std::string>& otherKeys) const;

    // the settings whose parameters these are, which notify a refusal of the parameters a call left while the lock was held
    void attachOwner(SettingsBase& own) noexcept { _owner = &own; }

    // the stream paths through the composite named `compositeName`, set again when a port is exported or withdrawn:
    // where each input enters and what its stream reaches. The marks of an earlier call leave `innerBlocks`, every
    // inner block's settings
    void setStreamPaths(std::string_view compositeName, std::vector<StreamEntry> entries, std::span<SettingsBase* const> innerBlocks);
    // a tag that enters at `entry`: each parameter whose tag key it carries at a new value, through the five steps. An
    // inner block the stream reaches takes the inner tag keys returned; every other inner block merges at the call
    [[nodiscard]] property_map translateTag(const SettingsBase& entry, const property_map& tagMap);

private:
    struct Prepared {
        std::vector<pmt::Value>           trial;
        std::vector<std::string>          parameters; // the parameters of the call
        std::vector<detail::InnerChanges> inners;
    };
    struct Forwarded {
        detail::InnerChanges                           inner;
        std::optional<std::expected<Values, Rejected>> answer{}; // the inner block's no-wait set
    };
    struct Committed {
        std::optional<detail::Merged> own;
        struct Inner {
            detail::InnerChanges                           inner;
            std::expected<detail::Merged, Rejected>        merged;
            std::optional<std::expected<Values, Rejected>> answer{}; // the finish's answer, or the commit's refusal
        };
        std::vector<Inner> inners;
    };

    std::vector<recipe::ParameterDeclaration> _declarations;
    std::vector<recipe::Binding>              _bindings;
    InnerResolver                             _resolve; // each prepare finds the blocks it sets in the graph as it stands

    std::string              _tagKeyPrefix; // of every inner tag key of this composite
    std::vector<StreamEntry> _entries;      // under the composite lock

    mutable std::mutex       _lock;       // the composite lock: the parameters, their record and tag keys
    std::vector<std::string> _tagKeys;    // the tag key that sets each parameter, empty for none; written under the latest lock as well
    mutable std::mutex       _latestLock; // over `_latest`, and `_tagKeys` for a tag that finds the composite lock held; no lock is taken in its hold
    property_map             _latest{};   // the latest value of each parameter a set or tag that does not wait left while the composite lock was held
    SettingsBase*            _owner = nullptr;
    std::vector<pmt::Value>  _parameters;
    ValueMap                 _record{};

    [[nodiscard]] std::optional<std::size_t> indexOf(std::string_view name) const noexcept;
    void                                     split(const property_map& changes, property_map& parameters, property_map& ownKeys) const;
    // under the composite lock, with each inner block's check taking its block lock up to `deadline`
    [[nodiscard]] std::expected<Prepared, Rejected> prepare(const property_map& parameters, std::optional<Clock::time_point> deadline, Clock::time_point calledAt) const;
    // the steps of a prepare that take no block lock: the conversions, the bindings and their inner blocks
    [[nodiscard]] std::expected<Prepared, Rejected> derive(const property_map& parameters) const;
    // the last step: each inner block checks the settings it receives
    [[nodiscard]] static std::expected<Prepared, Rejected> checkInners(Prepared prepared, std::optional<Clock::time_point> deadline, Clock::time_point calledAt);
    [[nodiscard]] std::expected<Values, Rejected>          setWithoutWait(SettingsBase& own, const property_map& changes);
    // under the composite lock: stores the parameters, then passes each inner block its keys as a no-wait set
    [[nodiscard]] std::vector<Forwarded> storeAndForward(Prepared prepared);
    // the parameters left while the lock was held, passed on as one batch when this thread takes the lock; never waits
    void forwardLatest();
    // under the composite lock: the parameters left while it was held, passed on as one batch
    void forwardLatestHeld();
    void translateHeld(const SettingsBase& entry, const property_map& tagMap, property_map& changes, property_map& translated, std::vector<std::pair<SettingsBase*, detail::Merged>>& staged);
    // the own keys' check, under their block lock taken up to `deadline`
    [[nodiscard]] static Rejected                    ownRefusals(SettingsBase& own, const property_map& ownKeys, std::optional<Clock::time_point> deadline, Clock::time_point calledAt);
    [[nodiscard]] std::expected<Committed, Rejected> prepareAndCommit(SettingsBase& own, const property_map& changes, std::vector<std::string>& parameters, std::optional<Clock::time_point> deadline, Clock::time_point calledAt);
};

} // namespace gr::settings::detail

#endif // GNURADIO_COMPOSITE_SETTINGS_HPP
