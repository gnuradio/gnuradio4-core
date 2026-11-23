#include <gnuradio-4.0/CompositeSettings.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <utility>

#include "SettingsHelpers.hpp"

namespace gr::settings::detail {

namespace {

// a parameter's value in its declared type: a number by the rule of a key of that type, a vector element by element, a
// text or a bool exactly
[[nodiscard]] std::expected<pmt::Value, std::string> convertParameter(const recipe::ParameterDeclaration& declaration, const pmt::Value& sent) {
    const std::string_view type    = declaration.type;
    const bool             vector  = type.ends_with("[]");
    const std::string_view element = vector ? type.substr(0UZ, type.size() - 2UZ) : type;
    if (element == "string" || (!vector && element == "bool")) {
        if (std::expected<void, gr::Error> agrees = recipe::checkParameterValue(declaration, sent); !agrees) {
            return std::unexpected(agrees.error().message);
        }
        return sent;
    }
    const std::optional<SettingType> settingType = parseTypeName(vector ? std::format("{}_vector", element) : std::string(element));
    if (!settingType) {
        return std::unexpected(std::format("the parameter '{}' declares the type {}, which converts no value", declaration.name, type));
    }
    return convert(Declaration{.name = declaration.name, .type = *settingType}, sent);
}

// the indices of the parameters a binding reads
[[nodiscard]] std::vector<std::size_t> parametersRead(const recipe::Binding& binding) {
    std::vector<std::size_t> read;
    const auto               add = [&read](const recipe::Expression& expression) {
        for (const recipe::Expression::Op& op : expression.ops) {
            if (op.kind == recipe::Expression::OpKind::pushParameter && std::ranges::find(read, op.parameterIndex) == read.end()) {
                read.push_back(op.parameterIndex);
            }
        }
    };
    if (binding.substituted) {
        read.push_back(*binding.substituted);
    } else if (!binding.sequence.empty()) {
        for (const recipe::SequenceElement& element : binding.sequence) {
            if (element.expression) {
                add(*element.expression);
            }
        }
    } else {
        add(binding.expression);
    }
    return read;
}

[[nodiscard]] std::string innerReason(std::string_view block, std::string_view key, std::string_view reason) { return std::format("block '{}': {}: {}", block, key, reason); }

// the parameters of the call behind an inner key; every parameter that reaches the block for an entry that names no key
[[nodiscard]] std::vector<std::string> parametersBehind(const detail::InnerChanges& inner, std::string_view key) {
    if (const auto it = inner.parametersOf.find(key); it != inner.parametersOf.end()) {
        return it->second;
    }
    std::vector<std::string> every;
    for (const auto& [_, parameters] : inner.parametersOf) {
        for (const std::string& parameter : parameters) {
            if (std::ranges::find(every, parameter) == every.end()) {
                every.push_back(parameter);
            }
        }
    }
    return every;
}

// the result of a call on parameters: each parameter of the call by the answers of the inner blocks behind it, with the
// own keys' answer joined. Each inner entry holds its inner changes and its answer; an inner block that refused the
// call at its commit fails each parameter behind it. A parameter's value entry carries the parameter's stored value,
// and the inner blocks' values in its reason
template<typename TInner>
[[nodiscard]] Values compositeResult(std::span<const std::string> parameters, const Values& stored, std::span<const TInner> inners, const Values& ownAnswer) {
    Values values;
    for (const std::string& parameter : parameters) {
        std::optional<std::string_view> pendingCause; // of the inner blocks' causes, the lowest ranked
        std::string                     failure;
        std::string                     actual;
        const auto                      join = [](std::string& text, const std::string& part) { text += text.empty() ? part : "; " + part; };
        for (const TInner& inner : inners) {
            if (!inner.answer->has_value()) {
                for (const Rejection& entry : inner.answer->error()) {
                    if (std::ranges::contains(parametersBehind(inner.inner, entry.key), parameter)) {
                        join(failure, innerReason(inner.inner.blockName, entry.key, entry.reason));
                    }
                }
                continue;
            }
            for (const auto& [key, entry] : **inner.answer) {
                if (const auto behind = inner.inner.parametersOf.find(key); behind == inner.inner.parametersOf.end() || std::ranges::find(behind->second, parameter) == behind->second.end()) {
                    continue;
                }
                if (entry.isFailed()) {
                    join(failure, innerReason(inner.inner.blockName, key, entry.reason));
                } else if (entry.isPending()) {
                    // "not running" first, then the work call, the queue and a timeout; any other cause ranks as a timeout
                    constexpr std::array<std::string_view, 4UZ> kRanked{kPendingNotRunning, kPendingOnWorker, kPendingQueued, kPendingTimeout};
                    const auto                                  ranked = std::ranges::find(kRanked, std::string_view(entry.reason));
                    const std::string_view                      cause  = ranked != kRanked.end() ? *ranked : kPendingTimeout;
                    if (!pendingCause || std::ranges::find(kRanked, cause) < std::ranges::find(kRanked, *pendingCause)) {
                        pendingCause = cause;
                    }
                } else if (entry.isValue()) {
                    join(actual, std::format("block '{}' key '{}' runs {}{}", inner.inner.blockName, key, entry.value, entry.reason.empty() ? std::string{} : std::format(" ({})", entry.reason)));
                }
            }
        }
        if (!failure.empty()) {
            values.insert_or_assign(parameter, Value{.state = Value::State::failed, .value = {}, .reason = std::move(failure)});
        } else if (pendingCause) {
            values.insert_or_assign(parameter, Value{.state = Value::State::pending, .value = {}, .reason = std::string(*pendingCause)});
        } else if (!actual.empty()) {
            const auto storedValue = stored.find(parameter);
            values.insert_or_assign(parameter, Value{.state = Value::State::value, .value = storedValue != stored.end() ? storedValue->second.value : pmt::Value{}, .reason = std::move(actual)});
        }
    }
    values.merge(Values(ownAnswer));
    return values;
}

} // namespace

CompositeSettings::CompositeSettings(std::vector<recipe::ParameterDeclaration> declarations, std::vector<recipe::Binding> bindings, std::vector<pmt::Value> values, const property_map& given, InnerResolver resolve) : _declarations(std::move(declarations)), _bindings(std::move(bindings)), _resolve(std::move(resolve)), _tagKeys(_declarations.size()), _parameters(std::move(values)) {
    for (const auto& [name, value] : given) {
        if (const std::optional<std::size_t> index = indexOf(name); index && *index < _parameters.size()) {
            _record.insert_or_assign(std::string(std::string_view(name)), _parameters[*index]);
        }
    }
}

std::optional<std::size_t> CompositeSettings::indexOf(std::string_view name) const noexcept {
    const auto found = std::ranges::find(_declarations, name, &recipe::ParameterDeclaration::name);
    return found != _declarations.end() ? std::optional(static_cast<std::size_t>(std::distance(_declarations.begin(), found))) : std::nullopt;
}

bool CompositeSettings::declares(std::string_view name) const noexcept { return indexOf(name).has_value(); }

bool CompositeSettings::namesParameter(const property_map& changes) const noexcept {
    return std::ranges::any_of(changes, [this](const auto& entry) { return declares(entry.first); });
}

void CompositeSettings::couple(std::string_view parameter, std::string tagKey) {
    if (const std::optional<std::size_t> index = indexOf(parameter); index) {
        std::lock_guard lock(_lock);
        std::lock_guard latestLock(_latestLock);
        _tagKeys[*index] = std::move(tagKey);
    }
}

property_map CompositeSettings::couplings() const {
    std::lock_guard lock(_lock);
    property_map    table;
    for (std::size_t index = 0UZ; index < _tagKeys.size(); ++index) {
        if (!_tagKeys[index].empty()) {
            table.insert_or_assign(std::pmr::string(_declarations[index].name), pmt::Value(_tagKeys[index]));
        }
    }
    return table;
}

std::vector<std::string> CompositeSettings::parameterNames() const {
    std::vector<std::string> names;
    std::ranges::transform(_declarations, std::back_inserter(names), &recipe::ParameterDeclaration::name);
    return names;
}

bool CompositeSettings::isGiven(std::string_view name) const {
    std::lock_guard lock(_lock);
    return _record.contains(name);
}

property_map CompositeSettings::record() const {
    std::lock_guard lock(_lock);
    property_map    given;
    for (const auto& [name, value] : _record) {
        given.insert_or_assign(std::pmr::string(name), value);
    }
    return given;
}

void CompositeSettings::split(const property_map& changes, property_map& parameters, property_map& ownKeys) const {
    for (const auto& [key, value] : changes) {
        (declares(key) ? parameters : ownKeys).insert_or_assign(key, value);
    }
}

std::expected<CompositeSettings::Prepared, Rejected> CompositeSettings::prepare(const property_map& parameters, std::optional<Clock::time_point> deadline, Clock::time_point calledAt) const {
    return derive(parameters).and_then([deadline, calledAt](Prepared prepared) { return checkInners(std::move(prepared), deadline, calledAt); });
}

std::expected<CompositeSettings::Prepared, Rejected> CompositeSettings::derive(const property_map& parameters) const {
    Prepared          prepared{.trial = _parameters, .parameters = {}, .inners = {}};
    Rejected          entries;
    std::vector<bool> inCall(_declarations.size(), false);
    for (const auto& [name, sent] : parameters) {
        const std::size_t                      index     = *indexOf(name);
        std::expected<pmt::Value, std::string> converted = convertParameter(_declarations[index], sent);
        if (!converted) {
            entries.push_back(invalidEntry(std::string(std::string_view(name)), converted.error()));
            continue;
        }
        prepared.trial[index] = std::move(*converted);
        inCall[index]         = true;
        prepared.parameters.emplace_back(std::string_view(name));
    }
    if (!entries.empty()) {
        return std::unexpected(std::move(entries));
    }

    for (const recipe::Binding& binding : _bindings) {
        std::vector<std::string> fromCall;
        for (const std::size_t index : parametersRead(binding)) {
            if (index < inCall.size() && inCall[index]) {
                fromCall.push_back(_declarations[index].name);
            }
        }
        if (fromCall.empty()) {
            continue;
        }
        std::expected<pmt::Value, gr::Error> derived = recipe::bindingValue(binding, prepared.trial);
        if (!derived) {
            for (const std::string& parameter : fromCall) {
                entries.push_back(invalidEntry(parameter, derived.error().message));
            }
            continue;
        }
        std::expected<InnerTarget, std::string> resolved = _resolve(binding.namePath);
        if (!resolved) {
            for (const std::string& parameter : fromCall) {
                entries.push_back(invalidEntry(parameter, resolved.error()));
            }
            continue;
        }
        auto target = std::ranges::find(prepared.inners, resolved->settings, &detail::InnerChanges::settings);
        if (target == prepared.inners.end()) {
            target = prepared.inners.insert(prepared.inners.end(), detail::InnerChanges{.settings = resolved->settings, .blockName = std::move(resolved->blockName), .changes = {}, .parametersOf = {}});
        }
        target->changes.insert_or_assign(std::pmr::string(binding.settingKey), std::move(*derived));
        target->parametersOf.insert_or_assign(binding.settingKey, std::move(fromCall));
    }
    if (!entries.empty()) {
        return std::unexpected(std::move(entries));
    }
    return prepared;
}

std::expected<CompositeSettings::Prepared, Rejected> CompositeSettings::checkInners(Prepared prepared, std::optional<Clock::time_point> deadline, Clock::time_point calledAt) {
    Rejected entries;
    for (const detail::InnerChanges& inner : prepared.inners) {
        if (std::expected<void, Rejected> checked = inner.settings->checkWithin(inner.changes, deadline, calledAt); !checked) {
            for (const Rejection& entry : checked.error()) {
                for (const std::string& parameter : parametersBehind(inner, entry.key)) {
                    entries.push_back(invalidEntry(parameter, innerReason(inner.blockName, entry.key, entry.reason)));
                }
            }
        }
    }
    if (!entries.empty()) {
        return std::unexpected(std::move(entries));
    }
    return prepared;
}

Rejected CompositeSettings::ownRefusals(SettingsBase& own, const property_map& ownKeys, std::optional<Clock::time_point> deadline, Clock::time_point calledAt) {
    if (ownKeys.empty()) {
        return {};
    }
    const SettingsBase::BlockLock ownLock(own, deadline);
    if (!ownLock.held()) {
        return own.lockTimedOut(ownKeys, calledAt);
    }
    const std::expected<CheckedCall, Rejected> checked = own.checkChanges(ownKeys);
    return checked ? Rejected{} : checked.error();
}

std::expected<CompositeSettings::Committed, Rejected> CompositeSettings::prepareAndCommit(SettingsBase& own, const property_map& changes, std::vector<std::string>& parameters, std::optional<Clock::time_point> deadline, Clock::time_point calledAt) {
    property_map parameterChanges;
    property_map ownKeys;
    split(changes, parameterChanges, ownKeys);
    const std::lock_guard lock(_lock); // from the prepare to the store, before any block lock
    forwardLatestHeld();               // the parameters a no-wait call left while the lock was held come first
    std::expected<Prepared, Rejected> prepared = prepare(parameterChanges, deadline, calledAt);
    if (!prepared) {
        Rejected entries = std::move(prepared.error());
        std::ranges::move(ownRefusals(own, ownKeys, deadline, calledAt), std::back_inserter(entries));
        return std::unexpected(std::move(entries));
    }
    // each block's check and merge in one hold of its block lock, taken up to the call's deadline
    const auto checkAndMerge = [deadline, calledAt](SettingsBase& settings, const property_map& keys) -> std::expected<Merged, Rejected> {
        const SettingsBase::BlockLock blockLock(settings, deadline);
        if (!blockLock.held()) {
            return std::unexpected(settings.lockTimedOut(keys, calledAt));
        }
        return settings.checkAndMerge(keys, nullptr, false);
    };
    Committed committed;
    if (!ownKeys.empty()) { // the own keys merge first, so own keys a call refuses leave every inner block as it was
        std::expected<Merged, Rejected> merged = checkAndMerge(own, ownKeys);
        if (!merged) {
            return std::unexpected(std::move(merged.error()));
        }
        committed.own = std::move(*merged);
    }
    for (const detail::InnerChanges& inner : prepared->inners) {
        // an inner block a direct call moved since its check may refuse here; the blocks merged before it keep theirs
        committed.inners.push_back(Committed::Inner{.inner = inner, .merged = checkAndMerge(*inner.settings, inner.changes)});
    }
    _parameters = prepared->trial;
    for (const std::string& name : prepared->parameters) {
        _record.insert_or_assign(name, _parameters[*indexOf(name)]);
    }
    parameters = std::move(prepared->parameters);
    return committed;
}

std::expected<void, Rejected> CompositeSettings::check(SettingsBase& own, const property_map& changes, std::optional<Clock::time_point> deadline, Clock::time_point calledAt) {
    property_map parameterChanges;
    property_map ownKeys;
    split(changes, parameterChanges, ownKeys);
    std::unique_lock                  lock(_lock);
    std::expected<Prepared, Rejected> prepared = prepare(parameterChanges, deadline, calledAt);
    Rejected                          entries  = prepared ? Rejected{} : std::move(prepared.error());
    std::ranges::move(ownRefusals(own, ownKeys, deadline, calledAt), std::back_inserter(entries));
    lock.unlock();
    forwardLatest();
    if (!entries.empty()) {
        return std::unexpected(std::move(entries));
    }
    return {};
}

std::expected<Values, Rejected> CompositeSettings::set(SettingsBase& own, const property_map& changes, Clock::time_point calledAt, Timeout timeout) {
    if (!own.waitsForLock(timeout)) {
        return setWithoutWait(own, changes);
    }
    std::vector<std::string>           parameters;
    std::expected<Committed, Rejected> committed = prepareAndCommit(own, changes, parameters, own.lockDeadline(calledAt, timeout), calledAt);
    forwardLatest();
    if (!committed) {
        return std::unexpected(std::move(committed.error()));
    }
    // each finish runs with the composite lock released, so the composite's other calls do not wait for an inner apply
    for (Committed::Inner& inner : committed->inners) {
        inner.answer = inner.merged ? std::expected<Values, Rejected>(inner.inner.settings->finish(*inner.merged, calledAt, timeout)) : std::unexpected(inner.merged.error());
    }
    const Values             ownAnswer = committed->own ? own.finish(*committed->own, calledAt, timeout) : Values{};
    std::vector<std::string> notParameters;
    return compositeResult(parameters, readParameters(parameters, notParameters), std::span<const Committed::Inner>(committed->inners), ownAnswer);
}

std::expected<Values, Rejected> CompositeSettings::setWithoutWait(SettingsBase& own, const property_map& changes) {
    property_map parameterChanges;
    property_map ownKeys;
    split(changes, parameterChanges, ownKeys);
    std::vector<std::string> parameters;
    std::vector<Forwarded>   forwarded;
    Values                   ownAnswer;
    const auto               setOwnKeys = [&]() -> std::expected<void, Rejected> {
        if (ownKeys.empty()) {
            return {};
        }
        std::expected<Values, Rejected> answer = own.set(ownKeys, noWait);
        if (!answer) {
            return std::unexpected(std::move(answer.error()));
        }
        ownAnswer = std::move(*answer);
        return {};
    };
    std::unique_lock lock(_lock, std::try_to_lock);
    if (!lock.owns_lock()) { // a holder may wait for an inner block: the latest value of each parameter waits for it to let go
        if (std::expected<void, Rejected> ownSet = setOwnKeys(); !ownSet) {
            return std::unexpected(std::move(ownSet.error()));
        }
        {
            const std::lock_guard latestLock(_latestLock);
            for (const auto& [name, value] : parameterChanges) {
                _latest.insert_or_assign(name, value);
            }
        }
        forwardLatest(); // the holder may have let go before the parameters were left
        Values pending = std::move(ownAnswer);
        for (const auto& [name, _] : parameterChanges) {
            pending.insert_or_assign(std::string(std::string_view(name)), Value{.state = Value::State::pending, .value = {}, .reason = std::string(kPendingQueued)});
        }
        return pending;
    }
    forwardLatestHeld(); // the parameters a call left while the lock was held come before this call's
    std::expected<Prepared, Rejected> derived = derive(parameterChanges).and_then([&](Prepared prepared) { return setOwnKeys().transform([&prepared] { return std::move(prepared); }); });
    Values                            stored; // read under this hold: a read after it would wait for the next holder, which may wait for an inner block
    if (derived) {
        parameters = derived->parameters;
        forwarded  = storeAndForward(std::move(*derived));
        for (const std::string& name : parameters) {
            stored.insert_or_assign(name, Value{.state = Value::State::value, .value = _parameters[*indexOf(name)], .reason = {}});
        }
    }
    lock.unlock();
    forwardLatest();
    if (!derived) {
        return std::unexpected(std::move(derived.error()));
    }
    return compositeResult(parameters, stored, std::span<const Forwarded>(forwarded), ownAnswer);
}

std::vector<CompositeSettings::Forwarded> CompositeSettings::storeAndForward(Prepared prepared) {
    _parameters = prepared.trial;
    for (const std::string& name : prepared.parameters) {
        _record.insert_or_assign(name, _parameters[*indexOf(name)]);
    }
    std::vector<Forwarded> forwarded;
    for (detail::InnerChanges& inner : prepared.inners) {
        std::expected<Values, Rejected> answer = inner.settings->set(inner.changes, noWait);
        if (!answer) { // the parameter stays stored: the refusal is the inner block's
            inner.settings->notifyRefused(answer.error());
        }
        forwarded.push_back(Forwarded{.inner = std::move(inner), .answer = std::move(answer)});
    }
    return forwarded;
}

void CompositeSettings::forwardLatest() {
    {
        const std::lock_guard latestLock(_latestLock);
        if (_latest.empty()) {
            return;
        }
    }
    std::unique_lock lock(_lock, std::try_to_lock);
    if (lock.owns_lock()) { // else its holder forwards them when it lets go
        forwardLatestHeld();
    }
}

void CompositeSettings::forwardLatestHeld() {
    property_map latest;
    {
        const std::lock_guard latestLock(_latestLock);
        latest = std::exchange(_latest, property_map{});
    }
    if (latest.empty()) {
        return;
    }
    // no caller waits for these parameters: a refused one is notified and leaves the batch, and the others derive once more
    std::expected<Prepared, Rejected> derived = derive(latest);
    if (!derived) {
        for (const Rejection& entry : derived.error()) {
            if (_owner != nullptr) {
                _owner->notify(NotificationLevel::error, NotificationKind::failed, entry.key, entry.reason);
            }
            if (const auto kept = latest.find(entry.key); kept != latest.end()) {
                latest.erase(kept);
            }
        }
        derived = latest.empty() ? std::move(derived) : derive(latest);
    }
    if (derived) {
        std::ignore = storeAndForward(std::move(*derived));
    }
}

Values CompositeSettings::readParameters(std::span<const std::string> keys, std::vector<std::string>& otherKeys) const {
    std::lock_guard lock(_lock);
    Values          values;
    const auto      entryOf = [this](std::size_t index) { return Value{.state = Value::State::value, .value = _parameters[index], .reason = {}}; };
    if (keys.empty()) {
        for (std::size_t index = 0UZ; index < _declarations.size() && index < _parameters.size(); ++index) {
            values.insert_or_assign(_declarations[index].name, entryOf(index));
        }
        return values;
    }
    for (const std::string& key : keys) {
        if (const std::optional<std::size_t> index = indexOf(key); index && *index < _parameters.size()) {
            values.insert_or_assign(key, entryOf(*index));
        } else {
            otherKeys.push_back(key);
        }
    }
    return values;
}

std::string innerTagKeyPrefix(std::string_view compositeName) { return std::format("{}{}/", kInnerTagKeyPrefix, compositeName); }

namespace {
// the tag key a composite adds to a tag that enters it for one inner setting
std::string innerTagKey(std::string_view compositePrefix, std::string_view blockName, std::string_view settingKey) { return std::format("{}{}.{}", compositePrefix, blockName, settingKey); }
} // namespace

void CompositeSettings::setStreamPaths(std::string_view compositeName, std::vector<StreamEntry> entries, std::span<SettingsBase* const> innerBlocks) {
    std::lock_guard lock(_lock);
    for (SettingsBase* inner : innerBlocks) {
        CompositeSettings* entered = this;
        if (inner->_compositeEntry.compare_exchange_strong(entered, nullptr)) {
            inner->_nTagReasons.fetch_sub(1UZ);
        }
        std::lock_guard innerLock(inner->_lock);
        inner->_nTagReasons.fetch_sub(std::erase_if(inner->_innerTagKeys, [this](const auto& entry) { return entry.first.starts_with(_tagKeyPrefix); }));
    }
    _tagKeyPrefix = innerTagKeyPrefix(compositeName);
    _entries      = std::move(entries);
    for (const StreamEntry& entry : _entries) {
        entry.entry->_nTagReasons.fetch_add(1UZ); // the count rises before the entry is set: a tag reads the count first
        if (entry.entry->_compositeEntry.exchange(this) != nullptr) {
            entry.entry->_nTagReasons.fetch_sub(1UZ);
        }
        for (const recipe::Binding& binding : _bindings) {
            std::expected<InnerTarget, std::string> target = _resolve(binding.namePath);
            if (target && std::ranges::find(entry.reached, target->settings) != entry.reached.end()) {
                std::lock_guard innerLock(target->settings->_lock);
                if (target->settings->_innerTagKeys.insert_or_assign(innerTagKey(_tagKeyPrefix, target->blockName, binding.settingKey), binding.settingKey).second) {
                    target->settings->_nTagReasons.fetch_add(1UZ);
                }
            }
        }
    }
}

property_map CompositeSettings::translateTag(const SettingsBase& entry, const property_map& tagMap) {
    property_map                                  changes; // a tag that carries no bound tag key copies and allocates nothing
    property_map                                  translated;
    std::vector<std::pair<SettingsBase*, Merged>> staged;
    {
        // the worker waits for no lock: while the composite lock is held, the latest value of each parameter the tag
        // carries waits for its holder to let go
        std::unique_lock lock(_lock, std::try_to_lock);
        if (!lock.owns_lock()) {
            const std::lock_guard latestLock(_latestLock);
            for (std::size_t index = 0UZ; index < _tagKeys.size(); ++index) {
                if (const auto tagged = _tagKeys[index].empty() ? tagMap.end() : tagMap.find(std::string_view(_tagKeys[index])); tagged != tagMap.end()) {
                    _latest.insert_or_assign(std::pmr::string(_declarations[index].name), tagged->second);
                }
            }
        } else {
            translateHeld(entry, tagMap, changes, translated, staged);
        }
    }
    for (auto& [settings, merged] : staged) {
        if (!settings->applyAtCall(merged) && merged.schedulerLink != nullptr) {
            merged.schedulerLink->wake();
        }
    }
    forwardLatest();
    return translated;
}

void CompositeSettings::translateHeld(const SettingsBase& entry, const property_map& tagMap, property_map& changes, property_map& translated, std::vector<std::pair<SettingsBase*, Merged>>& staged) {
    for (std::size_t index = 0UZ; index < _tagKeys.size(); ++index) {
        if (_tagKeys[index].empty()) {
            continue;
        }
        const auto tagged = tagMap.find(std::string_view(_tagKeys[index]));
        if (tagged == tagMap.end()) {
            continue;
        }
        // a tag at the stored value translates nothing; a value that does not convert stays for the check to refuse
        if (std::expected<pmt::Value, std::string> converted = convertParameter(_declarations[index], tagged->second); converted && sameValue(*converted, _parameters[index])) {
            continue;
        }
        changes.insert_or_assign(std::pmr::string(_declarations[index].name), tagged->second);
    }
    if (changes.empty()) {
        return;
    }
    const auto refuseTag = [](const SettingsBase& settings, const Rejected& refused) {
        for (const Rejection& refusal : refused) {
            settings.notify(NotificationLevel::error, NotificationKind::failed, refusal.key, std::format("tag value refused: {}", refusal.reason));
        }
    };
    std::expected<Prepared, Rejected> derived = derive(changes);
    if (!derived) {
        refuseTag(entry, derived.error());
        return;
    }
    // each inner block's lock is tried and held from its check to its merge. An inner block another thread holds, or a
    // nested composite, whose check waits for its own lock, takes its keys as a no-wait set instead
    std::vector<std::unique_ptr<SettingsBase::BlockLock>> held;
    std::vector<detail::InnerChanges>                     busy;
    std::vector<detail::InnerChanges>                     free;
    for (detail::InnerChanges& inner : derived->inners) {
        if (inner.settings->_composite == nullptr) {
            held.push_back(std::make_unique<SettingsBase::BlockLock>(*inner.settings, std::optional(Clock::now())));
            if (held.back()->held()) {
                free.push_back(std::move(inner));
                continue;
            }
            held.pop_back();
        }
        busy.push_back(std::move(inner));
    }
    derived->inners                            = std::move(free);
    std::expected<Prepared, Rejected> prepared = checkInners(std::move(*derived), std::nullopt, Clock::now());
    if (!prepared) {
        refuseTag(entry, prepared.error());
        return;
    }
    const auto entryAt = std::ranges::find(_entries, &entry, &StreamEntry::entry);
    for (const detail::InnerChanges& inner : prepared->inners) {
        if (entryAt != _entries.end() && std::ranges::find(entryAt->reached, inner.settings) != entryAt->reached.end()) {
            for (const auto& [key, value] : inner.changes) {
                translated.insert_or_assign(std::pmr::string(innerTagKey(_tagKeyPrefix, inner.blockName, key)), value);
            }
            continue;
        }
        // an inner block off the stream merges under the composite lock, so a tag's older value cannot land over a newer call's
        KeySet tagSets;
        for (const auto& [key, _] : inner.changes) {
            tagSets.emplace(std::string_view(key));
        }
        std::expected<Merged, Rejected> merged = inner.settings->checkAndMerge(inner.changes, &tagSets, false);
        if (!merged) {
            refuseTag(*inner.settings, merged.error());
            continue;
        }
        staged.emplace_back(inner.settings, std::move(*merged));
    }
    held.clear();
    for (const detail::InnerChanges& inner : busy) { // its value lands when that block's queue does, not at the tag's sample
        std::expected<Values, Rejected> answer = inner.settings->_composite != nullptr ? inner.settings->set(inner.changes, noWait) : inner.settings->slotChanges(inner.changes);
        if (!answer) {
            refuseTag(*inner.settings, answer.error());
        }
    }
    _parameters = prepared->trial; // a tag's value follows the stream and stores no parameter in the record
}

} // namespace gr::settings::detail
