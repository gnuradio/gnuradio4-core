#ifndef GNURADIO_SETTINGS_HPP
#define GNURADIO_SETTINGS_HPP

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gnuradio-4.0/SettingDeclaration.hpp>
#include <gnuradio-4.0/SettingNotification.hpp>
#include <gnuradio-4.0/SettingValue.hpp>
#include <gnuradio-4.0/SharedState.hpp>
#include <gnuradio-4.0/Tag.hpp>
#include <gnuradio-4.0/Value.hpp>

namespace gr {
struct SettingsBase;
} // namespace gr

namespace gr::settings {

// the prefix of the tag keys a recipe composite adds to a tag that enters it, one per inner setting a parameter
// drives; the composite's unique name follows it, and an output port that leaves that composite drops them
inline constexpr std::string_view kInnerTagKeyPrefix = "gr:composite:";

using ValueMap = std::map<std::string, pmt::Value, std::less<>>;
using KeySet   = std::set<std::string, std::less<>>;

template<typename TBlock>
struct BlockSettingsOf;

namespace detail {

struct CompositeSettings;

// the graph that holds a block, as its settings see it
struct GraphLink {
    virtual ~GraphLink() = default;
    // the name of the first port of `collection` at or past `count` that an edge holds, or nothing
    [[nodiscard]] virtual std::optional<std::string> edgeAtOrPast(std::string_view collection, std::size_t count) const = 0;
    // rebuilds what the graph derives from the block's ports after a port key resized a collection
    virtual void refreshPorts() = 0;
};

// the scheduler that holds a block's graph, as its settings see it; a block no scheduler holds has none
struct SchedulerLink {
    virtual ~SchedulerLink() = default;
    // wakes the idle workers, unless the scheduler neither runs nor starts or this thread is one of its workers
    virtual void wake() = 0;
    // holds a notification for the scheduler's next delivery
    virtual void push(Notification notification) = 0;
    // whether this thread is a worker of any scheduler, which waits for no apply, since the apply may need it
    [[nodiscard]] virtual bool isWorkerThread() const = 0;
    // the level of the nearest scheduler up that has one of its own, read at each notification
    [[nodiscard]] virtual std::optional<NotificationLevel> level() const = 0;
    // the wait of a call that names no timeout
    [[nodiscard]] virtual std::chrono::milliseconds defaultTimeout() const = 0;
    // the named block has left its scheduler for good, and the default reader at the top forgets its counts
    virtual void retire(std::string_view /*block*/) {}
    // a block of that name joins the scheduler, and the default reader at the top forgets the counts of the name
    virtual void join(std::string_view /*block*/) {}
    // delivers what the scheduler holds, for a scheduler nested in it that does not run and whose delivery no
    // message pass of its parent would carry on
    virtual void deliver() {}
};

enum class BlockState : std::uint8_t { stopped, running, paused, error };

struct PendingChange {
    pmt::Value value{};
    bool       given = false; // a caller sent the key; a tag gives none of the keys it sets
};

struct PendingChanges {
    std::map<std::string, PendingChange, std::less<>> keys{};
    bool                                              reset = false;

    [[nodiscard]] bool empty() const noexcept { return keys.empty() && !reset; }
};

// the value the block published by key for a setting it does not hold a member of, beside the value last set
struct PublishedValue {
    pmt::Value  value{};
    std::string reason{};
};

// the state a check reads: the block's own, under the block lock its caller holds through the merge
struct CheckState {
    const PendingChanges&                                     pending;
    const PendingChanges*                                     applying;
    const ValueMap&                                           inForce;
    const std::map<std::string, PublishedValue, std::less<>>& published; // each key's value the block published in place of the value last set
    GraphLink*                                                graphLink = nullptr;
    BlockState                                                state     = BlockState::stopped;
    bool                                                      starting  = false;
    bool                                                      setUp     = true; // false while the block's initialization checks its constructed values
};

struct Merged {
    ValueMap       sent{};
    std::uint64_t  batch         = 0U;
    GraphLink*     graphLink     = nullptr;
    SchedulerLink* schedulerLink = nullptr;
    BlockState     stateNoted    = BlockState::stopped;
    bool           waking        = false;
    bool           changesPorts  = false;
};

struct CheckedCall {
    ValueMap   converted{};
    KeySet     given{};
    KeySet     displaced{};
    bool       changesPorts = false;
    BlockState stateNoted   = BlockState::stopped;
};

struct Failure {
    std::uint64_t batch = 0U;
    std::string   reason{};
};

// a judgment of the block's setup; no entries when its settings passed
struct SetUpVerdict {
    Rejected entries{};
    bool     notReady = false; // the entries hold the block not ready; else they refuse the values given to it
    Merged   merged{};
};

// the key whose field in the block lies at `field`, or none
[[nodiscard]] std::optional<std::string_view> keyOfField(const DeclarationTable& table, void* block, const void* field);

// a hook names a key by a setting member of its block; a string that is no member names the key by its text
template<typename TName>
[[nodiscard]] std::string_view keyNamed(const DeclarationTable& table, void* block, const TName& name) {
    const std::optional<std::string_view> key = keyOfField(table, block, std::addressof(name));
    if constexpr (std::is_convertible_v<const TName&, std::string_view>) {
        return key.value_or(std::string_view(name));
    } else {
        assert(key.has_value() && "a settings hook names a setting member of its own block");
        return key.value_or(std::string_view{});
    }
}

// an argument that names a member; text as a literal, a pointer or a `std::string_view` takes the text form
template<typename T>
concept MemberName = !std::is_array_v<T> && !std::is_pointer_v<T> && !std::same_as<std::remove_cv_t<T>, std::string_view>;

// one member of the block's `Status` as a key and its value
struct StatusValue {
    std::string_view key;
    pmt::Value       value{};
};

// the latest start or resume of a block
struct Run {
    std::uint64_t              batch   = 0U;
    bool                       isStart = false;
    ValueMap                   keys{};     // each key the run covers with the value it wrote; an empty value for a key the device lacks
    Rejected                   refusals{}; // the whole-state check's
    std::optional<std::string> hookFailure{};
};

} // namespace detail

/**
 * @brief The state a call would produce over the pending changes and the values in force, which the check judges.
 *
 * At the block's initialization and at a start's check of the whole state every setting that holds a value is changed,
 * so a rule over several keys checks them all.
 */
class CallView {
    friend struct gr::SettingsBase;

    const detail::DeclarationTable* _table;
    const ValueMap*                 _converted;
    const KeySet*                   _displaced;
    const detail::CheckState*       _held;
    bool                            _wholeState;

    CallView(const detail::DeclarationTable& table, const ValueMap& converted, const KeySet& displaced, const detail::CheckState& held, bool wholeState = false) : _table(&table), _converted(&converted), _displaced(&displaced), _held(&held), _wholeState(wholeState || !held.setUp) {}

    [[nodiscard]] std::optional<pmt::Value> state(std::string_view key) const;
    [[nodiscard]] bool                      changed(std::string_view key) const; // the call holds the key at a value other than the one it would replace
};

} // namespace gr::settings

namespace gr {

namespace settings::detail {

// the settings a call would produce, which the check reads, each by its tag key; a key without a value holds none
struct Proposal {
    [[nodiscard]] virtual std::optional<pmt::Value> after(std::string_view tagKey) const = 0;

protected:
    ~Proposal() = default;
};

} // namespace settings::detail

/**
 * @brief What a check hook returns for settings it refuses: `Refused(member, reason)` names the setting at fault,
 * `Refused(reason)` names none, and `add` collects several. A refusal that holds no entry refuses nothing.
 *
 * A refusal that names no setting is reported against each setting the call changes; at the block's setup and at a
 * start it stays against the block.
 */
struct Refused {
    struct Entry {
        const void* field = nullptr; // the member named
        std::string key{};           // the key named by text, for an entry with no member
        std::string reason{};
    };

    std::vector<Entry> entries{};

    Refused() = default;
    explicit Refused(std::string_view reason) { add(reason); }
    Refused(std::string_view key, std::string_view reason) { add(key, reason); }
    template<settings::detail::MemberName TMember>
    Refused(const TMember& member, std::string_view reason) {
        add(member, reason);
    }

    void add(std::string_view reason) { entries.push_back(Entry{.reason = std::string(reason)}); }
    void add(std::string_view key, std::string_view reason) { entries.push_back(Entry{.key = std::string(key), .reason = std::string(reason)}); }
    // a text that is no member of the block names the key by its text
    template<settings::detail::MemberName TMember>
    void add(const TMember& member, std::string_view reason) {
        Entry entry{.field = std::addressof(member), .reason = std::string(reason)};
        if constexpr (std::is_convertible_v<const TMember&, std::string_view>) {
            entry.key = std::string(std::string_view(member));
        }
        entries.push_back(std::move(entry));
    }
};

namespace settings::detail {
struct FrameworkAccess;

/**
 * @brief Marks this thread as the block's own for the life of the object: the thread runs the block's work function,
 * one of its lifecycle hooks, `init()` or an apply.
 *
 * A merged block's work call marks each member as well. The hooks of a scheduler or a graph carry no mark, nor do the
 * stop and pause hooks of a blocking block that the scheduler runs while a worker may be inside its work call; a hook
 * that the block's work call runs carries the work call's mark. On its own thread a block's `set` and `resetDefaults`
 * are refused. The marks nest, one per block. The list of marks is held in gnuradio-shared-state (`ownThreadMarks()`),
 * so a check in the code of one shared object sees a mark that the code of another set on the same thread.
 */
struct OwnThread {
    const SettingsBase* settings;
    const OwnThread*    outer;

    // out of line: inlined into a caller's frame, the constructor's store of `this` in the thread-local list reads to
    // GCC as a dangling pointer, though the destructor puts back the outer mark before the object ends
    explicit OwnThread(const SettingsBase& blockSettings) noexcept;
    OwnThread(const OwnThread&)            = delete;
    OwnThread& operator=(const OwnThread&) = delete;
    ~OwnThread() { ownThreadMarks() = outer; }
};

/**
 * @brief Marks this thread as the own thread of each member of a merged block for the life of the object.
 *
 * `TMerged` calls `mark()` once for each of its members' settings, at most `kCount` times; the marks end in the
 * reverse order. At the end each member's `Status` is copied as the work call of a block used alone copies it.
 */
template<std::size_t kCount>
class OwnThreads {
    std::array<std::optional<OwnThread>, kCount> _marks{};
    std::array<SettingsBase*, kCount>            _members{};
    std::size_t                                  _nMarked = 0UZ;

public:
    template<typename TMerged>
    explicit OwnThreads(TMerged& merged) noexcept {
        merged.markMemberThreads(*this);
    }
    OwnThreads(const OwnThreads&)            = delete;
    OwnThreads& operator=(const OwnThreads&) = delete;
    ~OwnThreads();

    void mark(SettingsBase& memberSettings) noexcept {
        _members[_nMarked] = &memberSettings;
        _marks[_nMarked++].emplace(memberSettings);
    }
};
} // namespace settings::detail

/**
 * @brief The settings of one block: the pending changes, the values in force, the record of keys a caller gave, and
 * the calls that check, stage, apply and read them.
 *
 * Every change enters by one check and merge; one apply puts the pending changes in force and calls the block's settings
 * hook. A block no graph holds and that does not run applies each call on the caller's thread; a running block's
 * changes wait for its applying thread, which calls `applyStagedParameters()` before its work.
 *
 * A `set()` with `noWait`, or from a scheduler's worker thread, only tries the block lock; a composite passes its
 * parameters on to its inner blocks as such sets. While another thread holds the lock, the call puts its keys in a slot,
 * keeps the latest value of each key and returns them pending. The next merge, read, save, start or apply under the
 * lock checks and merges the slot as one call before its own work; a running block applies it at its next work call. A
 * refused key raises a failed notification, and the slot's other keys apply.
 */
struct SettingsBase {
private:
    using ValueMap = settings::ValueMap;
    using KeySet   = settings::KeySet;
    using Timeout  = settings::Timeout;
    using Values   = settings::Values;
    using Value    = settings::Value;
    using Rejected = settings::Rejected;

    friend struct settings::detail::FrameworkAccess;
    friend struct settings::detail::CompositeSettings;

    const settings::detail::DeclarationTable& _table;
    void*                                     _block;
    property_map*                             _described; // the block's map of described settings, or null
    property_map                              _constructedWith;
    ValueMap                                  _coreFields{}; // the fields of the keys the core declares, read and written under the block lock

    mutable std::timed_mutex             _lock;             // a waiting set() takes it within its timeout; one that does not wait only tries it
    mutable std::mutex                   _slotLock;         // over `_slot`, taken after the block lock and with no other lock taken in its hold
    property_map                         _slot{};           // the latest value of each key a set() that did not wait left while another thread held the block lock
    bool                                 _draining = false; // the slot's keys are under their check; block lock
    mutable std::mutex                   _settledLock;      // over `_settledCount`, which a waiting set() reads without the block lock
    mutable std::condition_variable      _settled;
    std::uint64_t                        _settledCount = 0U; // moves at each change a waiting set() judges again; written under both locks
    mutable std::atomic<std::thread::id> _lockHolder{};      // the thread that holds the block lock, whose hooks call back into these settings under its hold

    ValueMap                                                             _inForce{};
    ValueMap                                                             _toWrite{}; // an empty value removes the key's entry
    settings::detail::PendingChanges                                     _pending{};
    std::shared_ptr<const settings::detail::PendingChanges>              _applying{};
    ValueMap                                                             _record{};
    std::uint64_t                                                        _version       = 0U;
    std::uint64_t                                                        _nextBatch     = 1U;
    std::uint64_t                                                        _applyingBatch = 0U;
    std::uint64_t                                                        _appliedBatch  = 0U;
    std::map<std::string, settings::detail::Failure, std::less<>>        _failures{};
    std::map<std::string, settings::detail::PublishedValue, std::less<>> _published{}; // a key the block publishes by key alone, until the next write of it
    std::map<std::string, std::string, std::less<>>                      _couplings{}; // setting's tag key to the tag key that sets it
    std::atomic<std::uint8_t>                                            _ownLevel{kNoLevel};
    settings::detail::BlockState                                         _state       = settings::detail::BlockState::stopped;
    bool                                                                 _starting    = false;
    bool                                                                 _fieldsEnded = false; // the block's base destructor runs: its own members are destroyed
    ValueMap                                                             _storedDefaults{};
    bool                                                                 _setUp = false;
    Rejected                                                             _notReady{};     // what holds the block not ready after its setup; block lock
    std::optional<std::uint64_t>                                         _startChecked{}; // the version a scheduler's check before its start passed
    settings::detail::GraphLink*                                         _graphLink = nullptr;
    std::atomic<settings::detail::SchedulerLink*>                        _schedulerLink{nullptr}; // read without the block lock: a nested scheduler reads its parent's level and timeout through it
    std::atomic<bool>                                                    _hasWork{true};          // set while the block is not set up, so a work call on a block used alone initializes it
    property_map                                                         _toForward{};            // the applying thread's
    property_map*                                                        _forwarding   = nullptr; // the running settings hook's forwards; the applying thread's
    std::map<std::string, std::string, std::less<>>*                     _hookFailures = nullptr; // the keys the running apply hook fails, with the reason, which take back its batch; the applying thread's
    settings::detail::Run                                                _run{};
    settings::detail::CompositeSettings*                                 _composite = nullptr;     // the parameters of a recipe composite, whose own keys these settings are
    std::atomic<settings::detail::CompositeSettings*>                    _compositeEntry{nullptr}; // the composite whose input enters at this block
    std::map<std::string, std::string, std::less<>>                      _innerTagKeys{};          // inner tag key to the setting it sets; block lock
    std::atomic<std::size_t>                                             _nTagReasons{0UZ};        // the couplings, the inner tag keys and the composite entry: what makes the block read a tag
    mutable std::atomic<bool>                                            _statusWanted{false};     // a work call or a message pass copies the block's `Status` while set

    static constexpr std::uint8_t kNoLevel = 0xFFU;

protected:
    SettingsBase(const settings::detail::DeclarationTable& table, void* block, property_map* described, property_map constructedWith);
    // takes the state of `other`, whose block moved to `block`, as a moved block takes its settings
    SettingsBase(SettingsBase&& other, void* block, property_map* described) noexcept;

public:
    SettingsBase(const SettingsBase&)            = delete;
    SettingsBase& operator=(const SettingsBase&) = delete;
    SettingsBase& operator=(SettingsBase&&)      = delete;
    virtual ~SettingsBase()                      = default;

    [[nodiscard]] std::expected<Values, Rejected> set(const property_map& changes, Timeout timeout = std::nullopt);
    [[nodiscard]] std::expected<Values, Rejected> set(std::string_view key, pmt::Value value, Timeout timeout = std::nullopt);

    // Refuses as `set` would and stores nothing. On a block not yet set up it first runs the setup, apply hook included,
    // and is not const for that. Its own judgment runs no apply hook: a setup the apply hook refused stays refused until
    // the call gives a value that refusal names.
    [[nodiscard]] std::expected<void, Rejected> check(const property_map& changes);

    // nothing when the block is ready; else what holds it: a setting no caller gave a valid value, or the check's
    // refusal of the settings it holds. A `set` that makes the settings valid makes the block ready
    [[nodiscard]] std::expected<void, Rejected> ready();

    [[nodiscard]] std::expected<Value, Rejected>  get(std::string_view key) const;
    [[nodiscard]] std::expected<Values, Rejected> get(std::span<const std::string> keys = {}) const;

    template<typename T>
    [[nodiscard]] std::expected<T, Rejected> get(std::string_view key) const {
        return get(key).and_then([](const Value& entry) { return entry.as<T>(); }).transform_error([key](Rejected rejected) {
            for (settings::Rejection& entry : rejected) {
                entry.key = std::string(key);
            }
            return rejected;
        });
    }

    // the settings a call may name, block-wide and each member key the block holds; read-only keys are left out
    [[nodiscard]] std::set<std::string> writableMembers() const;

    // a set of the values in force when the block's initialization ended, then the reset hook
    std::expected<Values, Rejected> resetDefaults(Timeout timeout = std::nullopt);

    // couples settings to tag keys: setting to `domain:name`; an empty key removes the coupling; a tag that carries the
    // key sets the setting from its sample on
    [[nodiscard]] std::expected<void, Rejected> setTagCouplings(const property_map& couplings);
    [[nodiscard]] property_map                  tagCouplings() const;

    // stages `changes` as a `set` that does not wait; a refusal refuses every key, and the keys refused are returned
    [[nodiscard]] property_map setStaged(const property_map& changes);
    [[nodiscard]] property_map stagedParameters() const { return pendingValues(); } // each change waiting to apply
    [[nodiscard]] property_map activeParameters() const { return settings::heldValues(get()); }
    // the values a reset returns to, of `keys` or of every key; none for a key that has none
    [[nodiscard]] std::optional<property_map> getStored(std::span<const std::string> keys = {}) const;
    [[nodiscard]] std::optional<pmt::Value>   getStored(const std::string& key) const;
    void                                      storeDefaults() { storeInForceAsDefaults(); }
    // a member the settings hook assigns is read when the hook returns, so nothing is left to copy
    void updateActiveParameters() noexcept {}

protected:
    // the block author's calls, which `gr::Block` offers its block
    // the value the block now holds for `key`, from any of its threads: the key keeps it beside the value last set, with
    // `reason`, until the next write of the key, and a read returns it at once. From a settings hook it changes nothing
    // and notifies a failure: the apply hook assigns the setting
    void publish(std::string_view key, pmt::Value value, std::string_view reason = {});
    // the device refused `key`: in the apply hook, a failure of any key or status value takes back each key the batch
    // changed, as a throw does, and at the setup refuses the block as a refused constructed value does; in a start, it
    // marks a key the device lacks; elsewhere the key fails and keeps its value in force
    void fail(std::string_view key, std::string_view reason);
    // the members of the block's `Status`, on the block's own thread: each value that differs from the copy published
    // last becomes the copy a read returns, and one that replaces a value is announced
    void reportStatus(std::span<const settings::detail::StatusValue> values);
    // the copy of the block's `Status` a host reads, made after each apply hook and lifecycle hook and each message pass
    // in which a handler ran, and after a work call or another message pass while a watcher follows the status values,
    // a host read one since the last copy or a change on the scheduler's thread left it wanted
    void               copyStatus();
    [[nodiscard]] bool isGiven(std::string_view key) const;
    // from the settings hook, on the applying thread; what the hook forwards at the block's setup is dropped
    void forwardTag(std::string_view tagKey, pmt::Value value);

    // a block's settings initialize at its first `set`, `check` or work call; a graph that adds the block and a
    // scheduler built alone initialize them here, to refuse a constructed value at once. Settings that fail for a
    // reason other than a constructed value leave the block not ready, and `init()` succeeds
    std::expected<void, Rejected> init();
    [[nodiscard]] bool            heldNotReady() const;
    // a work call runs this before it reads its ports; with nothing pending it reads one atomic flag; it returns the failed keys
    Rejected applyStagedParameters() {
        if (!_hasWork.load(std::memory_order_acquire)) [[likely]] {
            return {};
        }
        return applyWhenDue();
    }
    // writes the block's metadata and described defaults from its declarations; checks nothing and calls no hook
    void describe();
    void moveState(settings::detail::BlockState state);
    void markStarting(bool starting); // a start runs from its first step to its last
    void endFields();                 // from the block's base destructor: no later call reads a reflected field
    // a start or a resume up to the start hook: the changes held while the block did not run apply, and a start's
    // check reads the whole state; returns that check's refusals
    [[nodiscard]] Rejected beginRun(bool isStart);
    // a scheduler's check of the block before any block of its graph starts: what holds the block not ready, else the
    // check of the whole state with the changes waiting to apply; kept as the latest start's refusals
    [[nodiscard]] Rejected checkStart();
    // the end of a start or a resume: the block runs, or stays stopped for a refusal, or is in error when the start hook threw
    void endRun(bool isStart, bool refused, std::optional<std::string> hookFailure);
    // each key of the latest run with its outcome
    [[nodiscard]] Values   runOutcome() const;
    [[nodiscard]] Rejected runRefusals() const; // the latest start's check refusals and the start hook's throw
    [[nodiscard]] bool     isRunning() const;
    // a graph's block applies on its worker only while it runs; a start or a resume applies what waited meanwhile
    [[nodiscard]] bool workerApplies() const;
    void               attachGraph(settings::detail::GraphLink* link);
    void               detachGraph(const settings::detail::GraphLink* link); // when `link` is the one attached
    void               attachScheduler(settings::detail::SchedulerLink* link);
    void               detachScheduler(settings::detail::SchedulerLink* link); // when `link` is the one attached
    property_map       takeForwarded();
    // the keys a saved graph writes: the record with the given keys waiting to apply over it; before `init()` the constructed values
    [[nodiscard]] property_map savedValues() const;
    [[nodiscard]] bool         hasWork() const noexcept { return _hasWork.load(std::memory_order_acquire); }

    // the check and the merge in one hold of the block lock. `tagSets` names the keys a tag sets, which the merge leaves
    // ungiven; it is null for any other source
    [[nodiscard]] std::expected<settings::detail::Merged, Rejected> checkAndMerge(const property_map& changes, const KeySet* tagSets, bool reset);
    // a tag's settings, each by its full key, and its reset mark; returns the refusals it notified
    Rejected tagEnters(const property_map& coupled, bool resetDefaults);
    // a tag as the stream carries it: each coupled key it holds, and its reset key
    Rejected tagEnters(const property_map& tagMap);

    // each refused key as an error notification, for a call no caller waits for
    void notifyRefused(const Rejected& refused) const;

private:
    // the check takes the block's `Settings` filled from the proposal
    [[nodiscard]] virtual bool                   hasCheckHook() const noexcept = 0;
    [[nodiscard]] virtual std::optional<Refused> runCheckHook(const settings::detail::Proposal& /*proposal*/) const { return std::nullopt; }
    [[nodiscard]] virtual bool                   hasApplyHook() const noexcept = 0;
    // the apply hook takes the block's own `Settings`, and may take the `Settings` before the change. `changed` holds the
    // keys whose value the batch changed. The values before the change are each written key's value a read returned in
    // `previousValues`, else the field in `fieldsBefore`, which is null at the setup
    [[nodiscard]] virtual bool applyTakesSettings() const noexcept { return false; }
    virtual void               runSettingsApplyHook(const property_map& /*changed*/, const ValueMap& /*previousValues*/, const ValueMap* /*fieldsBefore*/) {}
    // a merged block's path in place of the apply hook: each changed key at its value now in force goes to its members, and
    // a key a member refuses or fails fails the merged block's key
    [[nodiscard]] virtual std::expected<void, Rejected> runMembersApply(const property_map& /*changed*/) { return {}; }
    virtual void                                        runResetHook() = 0;
    // each writable setting of the block's `Settings` at the value a default-constructed `Settings` holds
    [[nodiscard]] virtual ValueMap    structDefaults() const { return {}; }
    virtual void                      publishStatus() {} // reads the block's `Status` and reports it
    virtual void                      describeBlock() = 0;
    [[nodiscard]] virtual bool        isSubgraph() const noexcept { return false; } // no work call reaches a subgraph's block
    [[nodiscard]] virtual std::string uniqueName() const = 0;

    // the block lock, taken unless this thread holds it already: a hook runs under the hold of its caller, and the calls
    // it makes into its own block's settings go on under that hold
    class BlockLock {
        const SettingsBase& _settings;
        bool                _owns = false;

    public:
        explicit BlockLock(const SettingsBase& settings) : _settings(settings) { lock(); }
        // takes the lock only up to `deadline`, none for no bound; `held()` tells whether it did
        BlockLock(const SettingsBase& settings, std::optional<std::chrono::steady_clock::time_point> deadline) : _settings(settings) { lockUntil(deadline); }
        BlockLock(const BlockLock&)            = delete;
        BlockLock& operator=(const BlockLock&) = delete;
        ~BlockLock() { unlock(); }

        void               lock();
        bool               lockUntil(std::optional<std::chrono::steady_clock::time_point> deadline);
        void               unlock();
        [[nodiscard]] bool held() const noexcept { return _settings.holdsLock(); }
    };

    struct Threshold {
        settings::NotificationLevel level = settings::NotificationLevel::warning;
        bool                        quiet = false; // an inherited "off" marks the notification for the readers a host installed alone
    };

    [[nodiscard]] std::optional<pmt::Value> readField(const settings::detail::KeyRef& ref, std::string_view tagKey) const;
    [[nodiscard]] bool                      writeField(const settings::detail::KeyRef& ref, std::string_view tagKey, const pmt::Value& value);
    void                                    eraseField(const settings::detail::KeyRef& ref, std::string_view tagKey);
    [[nodiscard]] ValueMap                  readFields(bool published) const;         // the published keys' fields, or every other key's
    [[nodiscard]] bool                      fieldsHold(const ValueMap& values) const; // readFields(false) would return `values` without its published keys
    void                                    rereadField(ValueMap& values, std::string_view key) const;
    [[nodiscard]] ValueMap                  sourceBeforeInit() const;
    void                                    eraseMembersPastCount(const settings::detail::KeyRef& countRef);
    [[nodiscard]] KeySet                    writePublished();
    [[nodiscard]] bool                      isPublishedKey(std::string_view key) const;
    [[nodiscard]] std::optional<pmt::Value> heldValue(std::string_view key) const; // the value a read of the key returns
    [[nodiscard]] bool                      holdsInForce(std::string_view tagKey, const pmt::Value& value) const;
    void                                    storeDefaultsFrom(const ValueMap& values);
    void                                    storeInForceAsDefaults();
    [[nodiscard]] property_map              pendingValues() const;
    [[nodiscard]] std::string               tagKeyOrSent(std::string_view key) const;

    [[nodiscard]] std::expected<void, Rejected> firstUse();
    // the setup over the constructed values with `changes` over them, merged and applied with `store` when it passes
    [[nodiscard]] settings::detail::SetUpVerdict setUpWith(const property_map& changes, bool store);
    // a `set` or a `check` on a block that is not set up, judged whole as its setup; none when the block is set up meanwhile
    [[nodiscard]] std::optional<std::expected<Values, Rejected>> setUpByCall(const property_map& changes, bool store);
    // `check()` with each hold of the block lock taken up to `deadline`, none for no bound; a composite's set calls it on
    // each inner block
    [[nodiscard]] std::expected<void, Rejected> checkWithin(const property_map& changes, std::optional<std::chrono::steady_clock::time_point> deadline, std::chrono::steady_clock::time_point calledAt);
    [[nodiscard]] Rejected                      applyWhenDue();
    // under the block lock the caller holds through its merge, with the check hook
    [[nodiscard]] std::expected<settings::detail::CheckedCall, Rejected> checkChanges(const property_map& changes);
    // the check hook over the state `view` reads
    [[nodiscard]] std::expected<void, Rejected> runCheck(const settings::CallView& view) const;
    [[nodiscard]] Rejected                      wholeStateRefusals();                                                          // the check hook's over the pending changes and the values in force
    [[nodiscard]] Rejected                      refusedDefaults(const KeySet& given) const;                                    // each declared default of a key not given that its own range refuses
    [[nodiscard]] settings::detail::Merged      merge(const settings::detail::CheckedCall& checked, bool fromTag, bool reset); // in the hold of the check
    [[nodiscard]] bool                          applyAtCall(const settings::detail::Merged& merged);
    Rejected                                    apply();
    [[nodiscard]] Values                        resultOf(std::uint64_t batch, const ValueMap& sent) const;
    [[nodiscard]] std::optional<std::string>    failedReason(std::string_view key, std::uint64_t batch, const pmt::Value& sent) const;
    [[nodiscard]] Values                        wait(const settings::detail::Merged& merged, std::optional<std::chrono::steady_clock::time_point> deadline);
    void                                        wakeWaiters(); // under the block lock, after a change a waiting set() judges
    // false for a call with `noWait` or a negative timeout, or on a scheduler's worker thread: it only tries the block lock
    [[nodiscard]] bool waitsForLock(Timeout timeout) const;
    // a call that does not wait found the block lock held: each key the declarations alone refuse refuses the call, else
    // the keys go to the slot and return pending
    [[nodiscard]] std::expected<Values, Rejected> slotChanges(const property_map& changes);
    // under the block lock, before a merge, a read or an apply: the slot's keys checked and merged as one call; none
    // before the setup or while an apply runs
    void drainSlot();
    void storeHasWork(bool hasWork); // under the slot lock, so a key the slot takes meanwhile keeps the flag raised
    // the end of a set()'s wait for the block lock and for its apply: its timeout, or the scheduler's default for one
    // that names none or does not wait; none past the clock's range
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>  lockDeadline(std::chrono::steady_clock::time_point calledAt, Timeout timeout) const;
    [[nodiscard]] Rejected                                              lockTimedOut(const property_map& changes, std::chrono::steady_clock::time_point calledAt) const;
    [[nodiscard]] Values                                                finish(const settings::detail::Merged& merged, std::chrono::steady_clock::time_point calledAt, Timeout timeout);
    [[nodiscard]] std::expected<Values, Rejected>                       readOwn(std::span<const std::string> keys) const;
    void                                                                notify(settings::NotificationLevel level, settings::NotificationKind what, std::string_view key, std::string_view reason) const;
    [[nodiscard]] std::optional<Threshold>                              threshold() const;
    [[nodiscard]] bool                                                  passes(settings::NotificationLevel level) const;
    [[nodiscard]] bool                                                  onOwnThread() const noexcept;
    [[nodiscard]] bool                                                  holdsLock() const noexcept;
    [[nodiscard]] std::vector<std::pair<std::string_view, std::size_t>> unsizedCollectionSizes() const;                   // each reflected port collection no port key sizes, with its size
    void                                                                restoreUnpublishedFields(const ValueMap& fields); // reports each reflected field in `fields` that differs from its value in force, and writes that value back
    void                                                                failLater(std::string_view tagKey, std::string_view reason);
    void                                                                failAtStart(std::string_view tagKey, std::string_view reason);
};

namespace settings::detail {

// the framework's entry points on a block's settings, for the block, its graph and its scheduler; no caller's call
struct FrameworkAccess {
    static std::expected<void, Rejected> init(SettingsBase& settings) { return settings.init(); }
    static Rejected                      applyStagedParameters(SettingsBase& settings) { return settings.applyStagedParameters(); }
    static void                          describe(SettingsBase& settings) { settings.describe(); }
    static void                          moveState(SettingsBase& settings, BlockState state) { settings.moveState(state); }
    static void                          endFields(SettingsBase& settings) { settings.endFields(); }
    static Rejected                      beginRun(SettingsBase& settings, bool isStart) { return settings.beginRun(isStart); }
    static Rejected                      checkStart(SettingsBase& settings) { return settings.checkStart(); }
    static void                          endRun(SettingsBase& settings, bool isStart, bool refused, std::optional<std::string> hookFailure) { settings.endRun(isStart, refused, std::move(hookFailure)); }
    static Values                        runOutcome(const SettingsBase& settings) { return settings.runOutcome(); }
    static Rejected                      runRefusals(const SettingsBase& settings) { return settings.runRefusals(); }
    static bool                          isRunning(const SettingsBase& settings) { return settings.isRunning(); }
    static bool                          workerApplies(const SettingsBase& settings) { return settings.workerApplies(); }
    static void                          markStarting(SettingsBase& settings, bool starting) { settings.markStarting(starting); }
    static void                          attachGraph(SettingsBase& settings, GraphLink* link) { settings.attachGraph(link); }
    static void                          detachGraph(SettingsBase& settings, const GraphLink* link) { settings.detachGraph(link); }
    static void                          attachScheduler(SettingsBase& settings, SchedulerLink* link) { settings.attachScheduler(link); }
    static void                          detachScheduler(SettingsBase& settings, SchedulerLink* link) { settings.detachScheduler(link); }
    static SchedulerLink*                schedulerLink(const SettingsBase& settings) noexcept { return settings._schedulerLink.load(std::memory_order_acquire); }
    // the level key's value in force when it names a level, read without the block lock
    static std::optional<NotificationLevel> ownLevel(const SettingsBase& settings) noexcept {
        const std::uint8_t own = settings._ownLevel.load(std::memory_order_relaxed);
        return own == SettingsBase::kNoLevel ? std::nullopt : std::optional(static_cast<NotificationLevel>(own));
    }
    static property_map        takeForwarded(SettingsBase& settings) { return settings.takeForwarded(); }
    static const property_map& constructedWith(const SettingsBase& settings) noexcept { return settings._constructedWith; }
    static property_map        savedValues(const SettingsBase& settings) { return settings.savedValues(); }
    static bool                hasWork(const SettingsBase& settings) noexcept { return settings.hasWork(); }
    static Rejected            tagEnters(SettingsBase& settings, const property_map& tagMap) { return settings.tagEnters(tagMap); }
    // `composite` holds the parameters of the recipe composite whose own keys `settings` are, and outlives every call on them
    static void                     attachComposite(SettingsBase& settings, CompositeSettings* composite) noexcept { settings._composite = composite; }
    static std::vector<std::string> parameterNames(const SettingsBase& settings); // a recipe composite's parameters, else none

    static void notifyRefused(const SettingsBase& settings, const Rejected& refused) { settings.notifyRefused(refused); }
    // the values in force become the ones a reset returns to
    static void storeDefaults(SettingsBase& settings) { settings.storeInForceAsDefaults(); }
    // each change waiting to apply, with its value
    [[nodiscard]] static property_map pendingValues(const SettingsBase& settings) { return settings.pendingValues(); }
    static void                       notify(const SettingsBase& settings, NotificationLevel level, NotificationKind what, std::string_view key, std::string_view reason) { settings.notify(level, what, key, reason); }

    // the block author's calls
    static void publish(SettingsBase& settings, std::string_view key, pmt::Value value, std::string_view reason = {}) { settings.publish(key, std::move(value), reason); }
    static void fail(SettingsBase& settings, std::string_view key, std::string_view reason) { settings.fail(key, reason); }
    template<MemberName TMember>
    static void fail(SettingsBase& settings, const TMember& member, std::string_view reason) {
        settings.fail(keyNamed(settings._table, settings._block, member), reason);
    }
    // copies the block's `Status` for a host, on the block's own thread
    static void copyStatus(SettingsBase& settings) { settings.copyStatus(); }
    // a watcher follows the block's `Status`, or a host read a status value since the last copy; one relaxed load
    [[nodiscard]] static bool statusWanted(const SettingsBase& settings) noexcept { return settings._statusWanted.load(std::memory_order_relaxed); }
    // the block's next work call or message pass copies its `Status`
    static void wantStatus(SettingsBase& settings) noexcept { settings._statusWanted.store(true, std::memory_order_relaxed); }
    static bool isGiven(const SettingsBase& settings, std::string_view key) { return settings.isGiven(key); }
    template<MemberName TMember>
    static bool isGiven(const SettingsBase& settings, const TMember& member) {
        return settings.isGiven(keyNamed(settings._table, settings._block, member));
    }
    static void forwardTag(SettingsBase& settings, std::string_view tagKey, pmt::Value value) { settings.forwardTag(tagKey, std::move(value)); }

    // the framework's calls on a scheduler, private to it
    template<typename TScheduler>
    static void onSettingsRunning(TScheduler& scheduler, std::function<void()> signal) {
        scheduler.onSettingsRunning(std::move(signal));
    }
    template<typename TScheduler>
    static void returnStartValues(TScheduler& scheduler, bool returned) {
        scheduler.returnStartValues(returned);
    }
    // the graph's call for a scheduler's start, private to the graph
    template<typename TGraph>
    static std::vector<std::string> blocksAddedWithoutInit(const TGraph& graph) {
        return graph.blocksAddedWithoutInit();
    }
};

/**
 * @brief The scope of a work call of a block that holds `Status`: at its end the block's `Status` is copied for a host
 * while a watcher follows it or a host read a status value since the last copy. The test is one load of one flag.
 */
struct StatusCopyAfterWork {
    SettingsBase& settings;

    explicit StatusCopyAfterWork(SettingsBase& blockSettings) noexcept : settings(blockSettings) {}
    StatusCopyAfterWork(const StatusCopyAfterWork&)            = delete;
    StatusCopyAfterWork& operator=(const StatusCopyAfterWork&) = delete;
    ~StatusCopyAfterWork() {
        if (FrameworkAccess::statusWanted(settings)) [[unlikely]] {
            FrameworkAccess::copyStatus(settings);
        }
    }
};

/**
 * @brief The scope of one lifecycle hook of a normal block. On the block's own thread it marks that thread and copies
 * the block's `Status` when the hook returns. A blocking block's stop and pause hooks run on the scheduler's thread
 * while a worker may be inside the work call: the block's next work call or message pass copies their values instead.
 */
struct LifecycleHook {
    std::optional<OwnThread> mark{};
    SettingsBase&            settings;

    LifecycleHook(SettingsBase& blockSettings, bool onOwnThread) noexcept : settings(blockSettings) {
        if (onOwnThread) {
            mark.emplace(blockSettings);
        }
    }
    LifecycleHook(const LifecycleHook&)            = delete;
    LifecycleHook& operator=(const LifecycleHook&) = delete;
    ~LifecycleHook() {
        if (mark) {
            FrameworkAccess::copyStatus(settings);
        } else {
            FrameworkAccess::wantStatus(settings);
        }
    }
};

template<std::size_t kCount>
OwnThreads<kCount>::~OwnThreads() {
    for (std::size_t member = 0UZ; member < _nMarked; ++member) {
        if (FrameworkAccess::statusWanted(*_members[member])) [[unlikely]] {
            FrameworkAccess::copyStatus(*_members[member]);
        }
    }
}

} // namespace settings::detail

} // namespace gr

namespace gr::settings {

namespace detail {

// a type that names a member, whatever its signature, makes `&Probe<T>::name` ambiguous
struct HookNames {
    int applySettings;
    int checkSettings;
};
template<typename T>
struct HookNameProbe : T, HookNames {};

// a final type takes no probe, so it is tested by the member's address, which misses an overloaded or templated member
template<typename T>
concept NamesApplySettings = std::is_class_v<T> && ((std::is_final_v<T> && requires { &T::applySettings; }) || (!std::is_final_v<T> && !requires { &HookNameProbe<T>::applySettings; }));
template<typename T>
concept NamesCheckSettings = std::is_class_v<T> && ((std::is_final_v<T> && requires { &T::checkSettings; }) || (!std::is_final_v<T> && !requires { &HookNameProbe<T>::checkSettings; }));

template<typename TBlock>
concept CheckTakesSettingsStruct = requires { static_cast<std::optional<Refused> (TBlock::*)(const typename TBlock::Settings&) const>(&TBlock::checkSettings); };

// the older settings hook: the values before and after the change, and the tags the hook forwards
template<typename TBlock>
concept SettingsChangedTakesMaps = requires(TBlock& block, const property_map& oldSettings, const property_map& newSettings) { block.settingsChanged(oldSettings, newSettings); };
template<typename TBlock>
concept SettingsChangedForwards = requires(TBlock& block, const property_map& oldSettings, const property_map& newSettings, property_map& forwardSettings) { block.settingsChanged(oldSettings, newSettings, forwardSettings); };

template<typename TBlock>
concept ApplyTakesNothing = requires { static_cast<void (TBlock::*)()>(&TBlock::applySettings); };
template<typename TBlock>
concept ApplyTakesSettings = requires { static_cast<void (TBlock::*)(typename TBlock::Settings&)>(&TBlock::applySettings); } || requires { static_cast<void (TBlock::*)(const typename TBlock::Settings&)>(&TBlock::applySettings); };
template<typename TBlock>
concept ApplyTakesSettingsAndOld = requires { static_cast<void (TBlock::*)(typename TBlock::Settings&, const typename TBlock::Settings&)>(&TBlock::applySettings); } || requires { static_cast<void (TBlock::*)(const typename TBlock::Settings&, const typename TBlock::Settings&)>(&TBlock::applySettings); };

// a scheduler's apply of the keys every scheduler holds, run before the scheduler's own apply hook
template<typename TBlock>
concept AppliesSchedulerSettings = requires(TBlock& scheduler, const typename TBlock::Settings& settings) { scheduler.applySchedulerSettings(settings, settings); };

// a merged block's own path, which hands the changed keys to its members and returns the keys they refuse or fail
template<typename TBlock>
concept AppliesToMembers = requires(TBlock& merged, const property_map& changed) {
    { merged.applyToMembers(changed) } -> std::same_as<std::expected<void, Rejected>>;
};

} // namespace detail

template<typename TBlock>
concept HasSettingsCheck = detail::CheckTakesSettingsStruct<TBlock>;

template<typename TBlock>
concept HasSettingsApply = detail::ApplyTakesSettings<TBlock> || detail::ApplyTakesSettingsAndOld<TBlock> || detail::SettingsChangedTakesMaps<TBlock> || detail::SettingsChangedForwards<TBlock>;

template<typename TBlock>
concept HasSettingsReset = requires(TBlock& block) { block.reset(); };

/**
 * @brief The settings of one block of type TBlock: its declarations, its fields and its hooks.
 *
 * Both hooks need the block to reflect one member of type `Settings`, whatever its name. The check hook runs on the
 * caller's thread under the block's settings lock: `std::optional<gr::Refused> checkSettings(const Settings& s) const`.
 * Core may call it often, for a `check` that stores nothing as for a `set`: it reads its argument alone, changes nothing
 * in the block, and returns quickly; the compiler refuses one that is not `const`. The settings hook runs on the
 * applying thread under that lock: `void applySettings(Settings& s)` or `void applySettings(Settings& s, const Settings& old)`, with `s` const
 * where the hook assigns nothing. `s` is the block's own member, which holds the new values; `old` holds the values
 * before them, and at the setup the declared defaults. For the hook's call `s.x.changed()` tells whether the batch
 * changed the `Annotated` setting `x`; at the setup every setting is changed. Each hook is optional, and `reset()` runs
 * after a reset's defaults apply. A rule about values lives in the check hook, which refuses the whole call before a
 * write; the apply hook fails a key, by `fail` or a throw, only where doing the thing fails. A throw from the check is
 * its refusal naming no setting, with the thrown message; one from the apply fails each key the call changed. A block
 * may define the older hook in place of the apply hook: `void settingsChanged(const property_map& old, const
 * property_map& new)`, with the changed settings' values before and after, or with a third `property_map&` whose
 * entries it forwards as a tag. On a scheduler the core's `applySchedulerSettings()` applies the keys of
 * `SchedulerSettings` before the scheduler's hook.
 */
template<typename TBlock>
struct BlockSettingsOf : SettingsBase {
private:
    TBlock&               _block;
    std::shared_ptr<void> _statusCopied{}; // the block's `Status` as reported last, held by pointer since `TBlock` is incomplete here

public:
    explicit BlockSettingsOf(TBlock& block, property_map constructedWith = {}) : SettingsBase(detail::declarationTable<TBlock>(), std::addressof(block), describedMapOf(block), std::move(constructedWith)), _block(block) {
        static_assert(!detail::ApplyTakesNothing<TBlock>, "the settings hook takes the block's settings, as `void applySettings(Settings& s)` or `void applySettings(Settings& s, const Settings& old)`");
        static_assert(!detail::NamesApplySettings<TBlock> || HasSettingsApply<TBlock> || detail::ApplyTakesNothing<TBlock>, "the settings hook is `void applySettings(Settings& s)` or `void applySettings(Settings& s, const Settings& old)`, with `s` const where it assigns nothing");
        static_assert(!(detail::ApplyTakesSettings<TBlock> || detail::ApplyTakesSettingsAndOld<TBlock>) || detail::holdsSettingsStruct<TBlock>(), "an apply hook that takes `Settings& s` needs the block to reflect a member of type `Settings`");
        static_assert(!detail::NamesCheckSettings<TBlock> || HasSettingsCheck<TBlock>, "the check hook is `std::optional<gr::Refused> checkSettings(const Settings& s) const`");
        static_assert(!detail::CheckTakesSettingsStruct<TBlock> || detail::holdsSettingsStruct<TBlock>(), "a check that takes `const Settings& s` needs the block to reflect a member of type `Settings`");
    }

    // the settings of a block moved to `newOwner`, taken from the settings of the block it moved from
    BlockSettingsOf(TBlock& newOwner, BlockSettingsOf&& other) noexcept : SettingsBase(std::move(other), std::addressof(newOwner), describedMapOf(newOwner)), _block(newOwner), _statusCopied(std::move(other._statusCopied)) {}

private:
    static property_map* describedMapOf(TBlock& block) {
        if constexpr (DescribedBlock<TBlock>) {
            return std::addressof(block.describedSettings);
        } else {
            return nullptr;
        }
    }

    [[nodiscard]] bool hasCheckHook() const noexcept override { return HasSettingsCheck<TBlock>; }

    [[nodiscard]] bool isSubgraph() const noexcept override {
        if constexpr (requires { TBlock::blockCategory; }) {
            using Category = std::remove_cvref_t<decltype(TBlock::blockCategory)>;
            return TBlock::blockCategory == Category::TransparentBlockGroup;
        } else {
            return false;
        }
    }

    [[nodiscard]] bool hasApplyHook() const noexcept override { return HasSettingsApply<TBlock> || detail::AppliesSchedulerSettings<TBlock> || detail::AppliesToMembers<TBlock>; }

    [[nodiscard]] std::expected<void, settings::Rejected> runMembersApply([[maybe_unused]] const property_map& changed) override {
        if constexpr (detail::AppliesToMembers<TBlock>) {
            return _block.applyToMembers(changed);
        } else {
            return {};
        }
    }

    [[nodiscard]] std::optional<Refused> runCheckHook([[maybe_unused]] const detail::Proposal& proposal) const override {
        if constexpr (detail::CheckTakesSettingsStruct<TBlock>) {
            return checkProposedStruct(proposal);
        } else {
            return std::nullopt;
        }
    }

    // the check judges a `Settings` filled from the proposal, the state the call would put in force, never a copy of the
    // block's own `Settings`; a refusal that names a member of the copy names the block's own member
    [[nodiscard]] std::optional<Refused> checkProposedStruct(const detail::Proposal& proposal) const {
        typename TBlock::Settings proposed{};
        detail::forEachFlatMember<TBlock>([&proposal, &proposed]<typename TMember>(TMember) {
            if constexpr (TMember::kInStruct && !TMember::kInStatus) {
                using RawType = std::remove_cvref_t<typename TMember::Type>;
                using Type    = std::remove_cv_t<typename detail::FieldValue<unwrap_if_wrapped_t<RawType>>::type>;
                if constexpr (detail::kIsSettingField<Type> && !std::is_const_v<typename TMember::Type> && !gr::meta::is_immutable<RawType>::value) {
                    if (const std::optional<pmt::Value> value = proposal.after(TMember::kName.view()); value) {
                        std::ignore = detail::writeMember(TMember::inStruct(proposed), *value);
                    }
                }
            }
        });
        std::optional<Refused> refused = std::as_const(_block).checkSettings(std::as_const(proposed));
        if (!refused) {
            return refused;
        }
        for (Refused::Entry& entry : refused->entries) {
            detail::forEachFlatMember<TBlock>([this, &entry, &proposed]<typename TMember>(TMember) {
                if constexpr (TMember::kInStruct && !TMember::kInStatus) {
                    if (entry.field == std::addressof(TMember::inStruct(proposed))) {
                        entry.field = std::addressof(TMember::of(_block));
                    }
                }
            });
        }
        return refused;
    }

    [[nodiscard]] bool applyTakesSettings() const noexcept override { return HasSettingsApply<TBlock> || detail::AppliesSchedulerSettings<TBlock>; }

    void runSettingsApplyHook([[maybe_unused]] const property_map& changed, [[maybe_unused]] const settings::ValueMap& previousValues, [[maybe_unused]] const settings::ValueMap* fieldsBefore) override {
        if constexpr ((detail::ApplyTakesSettings<TBlock> || detail::ApplyTakesSettingsAndOld<TBlock> || detail::AppliesSchedulerSettings<TBlock>) && detail::holdsSettingsStruct<TBlock>()) {
            typename TBlock::Settings& own = detail::settingsStructOf(_block);
            const ChangedForHook       marks(own, fieldsBefore == nullptr ? nullptr : &changed);
            if constexpr (detail::ApplyTakesSettingsAndOld<TBlock> || detail::AppliesSchedulerSettings<TBlock>) {
                const typename TBlock::Settings old = settingsBefore(previousValues, fieldsBefore);
                if constexpr (detail::AppliesSchedulerSettings<TBlock>) {
                    _block.applySchedulerSettings(own, old);
                }
                if constexpr (detail::ApplyTakesSettingsAndOld<TBlock>) {
                    _block.applySettings(own, old);
                } else if constexpr (detail::ApplyTakesSettings<TBlock>) {
                    _block.applySettings(own);
                }
            } else {
                _block.applySettings(own);
            }
        } else if constexpr (detail::SettingsChangedTakesMaps<TBlock> || detail::SettingsChangedForwards<TBlock>) {
            property_map oldSettings;
            for (const auto& [key, value] : previousValues) {
                oldSettings.insert_or_assign(std::pmr::string(key), value);
            }
            if constexpr (detail::SettingsChangedForwards<TBlock>) {
                property_map forwardSettings;
                _block.settingsChanged(oldSettings, changed, forwardSettings);
                for (auto& [key, value] : forwardSettings) {
                    forwardTag(key, std::move(value));
                }
            } else {
                _block.settingsChanged(oldSettings, changed);
            }
        }
    }

    // each `Annotated` setting of `own` reads changed for the life of the object: all of them for a null `changed`, else
    // those `changed` names. The flags clear when the hook returns or throws. `TSettings` is `TBlock::Settings`, named
    // as a template parameter since `TBlock` is incomplete where the class is instantiated
    template<typename TSettings>
    struct ChangedForHook {
        TSettings& own;

        ChangedForHook(TSettings& ownSettings, const property_map* changed) : own(ownSettings) {
            markChanged(own, [changed](std::string_view key) { return changed == nullptr || changed->contains(key); });
        }
        ChangedForHook(const ChangedForHook&)            = delete;
        ChangedForHook& operator=(const ChangedForHook&) = delete;
        ~ChangedForHook() {
            markChanged(own, [](std::string_view) { return false; });
        }
    };

    template<typename TSettings>
    static void markChanged(TSettings& own, auto isChanged) {
        detail::forEachFlatMember<TBlock>([&own, &isChanged]<typename TMember>(TMember) {
            if constexpr (TMember::kInStruct && !TMember::kInStatus && AnnotatedType<std::remove_cvref_t<typename TMember::Type>> && !std::is_const_v<std::remove_reference_t<typename TMember::Type>>) {
                TMember::inStruct(own)._changed = isChanged(TMember::kName.view());
            }
        });
    }

    // the `Settings` the block held before the change, converted from the values the apply restores on a throw; the
    // declared defaults at the setup
    [[nodiscard]] static auto settingsBefore(const settings::ValueMap& previousValues, const settings::ValueMap* fieldsBefore) {
        typename TBlock::Settings old{};
        if (fieldsBefore == nullptr) {
            return old;
        }
        detail::forEachFlatMember<TBlock>([&previousValues, fieldsBefore, &old]<typename TMember>(TMember) {
            if constexpr (TMember::kInStruct && !TMember::kInStatus) {
                using RawType = std::remove_cvref_t<typename TMember::Type>;
                using Type    = std::remove_cv_t<typename detail::FieldValue<unwrap_if_wrapped_t<RawType>>::type>;
                if constexpr (detail::kIsSettingField<Type> && !std::is_const_v<typename TMember::Type> && !gr::meta::is_immutable<RawType>::value) {
                    const std::string_view key      = TMember::kName.view();
                    const auto             previous = previousValues.find(key);
                    const auto             field    = fieldsBefore->find(key);
                    if (previous != previousValues.end()) {
                        assignStored(TMember::inStruct(old), previous->second);
                    } else if (field != fieldsBefore->end()) {
                        assignStored(TMember::inStruct(old), field->second);
                    }
                }
            }
        });
        return old;
    }

    // a value in force holds the field's own type, so a list of the field's element type is copied once, unchecked;
    // any other value converts as a write converts it
    template<typename TMember>
    static void assignStored(TMember& member, const pmt::Value& stored) {
        using Type = unwrap_if_wrapped_t<std::remove_cvref_t<TMember>>;
        if constexpr (gr::meta::vector_type<Type> && requires { stored.get_if<Tensor<typename Type::value_type>>(); }) {
            if (const Tensor<typename Type::value_type>* list = stored.get_if<Tensor<typename Type::value_type>>(); list != nullptr) {
                if constexpr (AnnotatedType<std::remove_cvref_t<TMember>>) {
                    member.value.assign(list->begin(), list->end());
                } else {
                    member.assign(list->begin(), list->end());
                }
                return;
            }
        }
        std::ignore = detail::writeMember(member, stored);
    }

    [[nodiscard]] settings::ValueMap structDefaults() const override {
        settings::ValueMap defaults;
        if constexpr (detail::holdsSettingsStruct<TBlock>()) {
            const typename TBlock::Settings constructed{};
            detail::forEachFlatMember<TBlock>([&constructed, &defaults]<typename TMember>(TMember) {
                if constexpr (TMember::kInStruct && !TMember::kInStatus) {
                    using RawType = std::remove_cvref_t<typename TMember::Type>;
                    using Type    = std::remove_cv_t<typename detail::FieldValue<unwrap_if_wrapped_t<RawType>>::type>;
                    if constexpr (detail::kIsSettingField<Type> && !std::is_const_v<typename TMember::Type> && !gr::meta::is_immutable<RawType>::value) {
                        defaults.insert_or_assign(std::string(TMember::kName.view()), detail::storedValue(detail::undecorated(TMember::inStruct(constructed))));
                    }
                }
            });
        }
        return defaults;
    }

    void runResetHook() override {
        if constexpr (HasSettingsReset<TBlock>) {
            _block.reset();
        }
    }

    // the block's member of its type `Status`
    [[nodiscard]] static const auto& statusOf(const TBlock& block) {
        constexpr std::size_t kIdx = static_cast<std::size_t>(std::ranges::find(detail::kOuterKinds<TBlock>, detail::OuterKind::status) - detail::kOuterKinds<TBlock>.begin());
        return refl::data_member<kIdx>(block);
    }

    // the status values that differ from their copy in `copied`, each then copied there; a type without `==` counts as
    // changed; a not-a-number that stays one, alone, in a list or in a complex number, does not. A value the block gave by
    // `publish()` on an unchanged key of its `Status` stays
    void collectChangedStatus(auto& copied, std::vector<detail::StatusValue>& values) {
        detail::forEachFlatMember<TBlock>([this, &copied, &values]<typename TMember>(TMember) {
            using Type = std::remove_cv_t<typename detail::FieldValue<unwrap_if_wrapped_t<std::remove_cvref_t<typename TMember::Type>>>::type>;
            if constexpr (TMember::kInStatus && detail::kIsSettingField<Type>) {
                const auto& now    = TMember::of(std::as_const(_block));
                auto&       before = TMember::inStruct(copied);
                if constexpr (std::equality_comparable<std::remove_cvref_t<decltype(detail::undecorated(now))>>) {
                    if (detail::sameNumber(detail::undecorated(now), detail::undecorated(before))) {
                        return;
                    }
                }
                values.push_back(detail::StatusValue{.key = TMember::kName.view(), .value = detail::storedValue(detail::undecorated(now))});
                before = now;
            }
        });
    }

    // every status value of the block
    void collectStatus(std::vector<detail::StatusValue>& values) const {
        detail::forEachFlatMember<TBlock>([this, &values]<typename TMember>(TMember) {
            using Type = std::remove_cv_t<typename detail::FieldValue<unwrap_if_wrapped_t<std::remove_cvref_t<typename TMember::Type>>>::type>;
            if constexpr (TMember::kInStatus && detail::kIsSettingField<Type>) {
                values.push_back(detail::StatusValue{.key = TMember::kName.view(), .value = detail::storedValue(detail::undecorated(TMember::of(std::as_const(_block))))});
            }
        });
    }

    // reports the status values that changed since the copy before, and every value at the first copy. The members are
    // compared in their own types: an unchanged `Status` costs no conversion to a `pmt::Value`, no allocation and no lock
    void publishStatus() override {
        if constexpr (detail::holdsStatusStruct<TBlock>()) {
            using Status = typename TBlock::Status;
            std::vector<detail::StatusValue> values;
            if constexpr (std::is_copy_constructible_v<Status> && std::is_copy_assignable_v<Status>) {
                if (_statusCopied == nullptr) {
                    _statusCopied = std::make_shared<Status>(statusOf(_block));
                    collectStatus(values);
                } else {
                    collectChangedStatus(*static_cast<Status*>(_statusCopied.get()), values);
                }
            } else {
                collectStatus(values);
            }
            if (!values.empty()) {
                reportStatus(values);
            }
        }
    }

    void describeBlock() override { detail::describe(_block); }

    [[nodiscard]] std::string uniqueName() const override {
        if constexpr (requires { std::string(detail::undecorated(_block.unique_name)); }) {
            return std::string(detail::undecorated(_block.unique_name));
        } else {
            return std::string(gr::meta::type_name<TBlock>());
        }
    }
};

} // namespace gr::settings

#endif // GNURADIO_SETTINGS_HPP
