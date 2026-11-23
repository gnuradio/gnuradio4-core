#include <gnuradio-4.0/Settings.hpp>

#include <gnuradio-4.0/CompositeSettings.hpp>

#include <gnuradio-4.0/Exception.hpp>

#include "SettingsHelpers.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <iterator>
#include <limits>
#include <thread>
#include <tuple>
#include <utility>

namespace gr::settings {

using namespace detail;
using detail::BlockState;
using detail::CheckedCall;
using detail::CheckState;
using detail::Merged;
using detail::PendingChange;
using detail::PendingChanges;

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::string_view kWrittenOutsideApply = "assigned outside applySettings()"; // the value in force is written back
constexpr std::string_view kAssignedDirectly    = "assigned directly";                // in place of a value given at construction or by set()

constexpr std::string_view kUndeclared     = "unknown setting";
constexpr std::string_view kNoValue        = "no value";
constexpr std::string_view kReadOnly       = "read-only";
constexpr std::string_view kStatus         = "read-only status";                       // a member of the block's `Status`, which the block reports
constexpr std::string_view kOwnCall        = "not allowed on the block's own thread";  // a block changes its own settings in applySettings()
constexpr std::string_view kPublishInApply = "publish not allowed in applySettings()"; // the hook assigns the setting
constexpr std::string_view kWhileStopped   = "not allowed while running";
constexpr std::string_view kWaitingForLock = "waiting for the block's settings"; // a call found the block lock held up to its timeout

[[nodiscard]] std::string tagKeyOf(const KeyRef& ref) {
    if (ref.group == nullptr) {
        return ref.declaration->name;
    }
    return std::format("{}.{}.{}", ref.group->name, ref.member, ref.declaration->name);
}

[[nodiscard]] std::optional<std::string> canonicalKey(const DeclarationTable& table, std::string_view key) {
    const std::optional<KeyRef> ref = table.find(key);
    return ref ? std::optional<std::string>(tagKeyOf(*ref)) : std::nullopt;
}

[[nodiscard]] bool hasDefault(const Declaration& declaration) { return declaration.source != DeclarationSource::described || declaration.defaultValue.has_value(); }

[[nodiscard]] std::uint64_t countFrom(const pmt::Value* value) { return value != nullptr ? detail::losslessAs<std::uint64_t>(*value).value_or(0U) : 0U; }

// a value convert() would return unchanged: a scalar of the declared type, a map, or a text with no enumerator to match
[[nodiscard]] bool holdsDeclaredType(const Declaration& declaration, const pmt::Value& value) {
    using enum pmt::Value::ContainerType;
    switch (declaration.type.container) {
    case Scalar: return value.container_type() == Scalar && value.value_type() == declaration.type.element;
    case String: return value.is_string() && declaration.enumType.empty();
    case Map: return value.is_map();
    default: return false;
    }
}

// the map's key string is built only for a key the map lacks
template<typename TMap, typename TValue>
void assignAt(TMap& map, std::string_view key, TValue value) {
    if (const auto it = map.find(key); it != map.end()) {
        it->second = std::move(value);
    } else {
        map.emplace(std::string(key), std::move(value));
    }
}

// the member groups a block-wide key counts
template<typename TVisitor>
void forEachCountedGroup(const DeclarationTable& table, std::string_view countKey, TVisitor&& visitor) {
    for (const auto& [_, group] : table.groups) {
        if (!group.countKey.empty() && group.countKey == countKey) {
            visitor(group);
        }
    }
}

// whether a tag key names a member of `group` at or past `count`
[[nodiscard]] bool isMemberPast(const DeclarationTable& table, std::string_view key, const MemberGroup& group, std::uint64_t count) {
    const std::optional<KeyRef> ref = table.find(key);
    return ref && !ref->bareName && ref->group == &group && ref->member >= count;
}

// the value a check reads for a key: the pending change, the change being applied, else the value a read returns, which
// is the value the block published by key in place of the value last set where it published one
[[nodiscard]] std::optional<pmt::Value> lookUp(const DeclarationTable& table, const CheckState& held, std::string_view tagKey, bool withPending) {
    const std::optional<KeyRef> ref = table.find(tagKey);
    if (!ref || ref->declaration->isReadOnly()) {
        return std::nullopt;
    }
    if (withPending) {
        if (const auto it = held.pending.keys.find(tagKey); it != held.pending.keys.end()) {
            return it->second.value;
        }
        if (held.applying != nullptr) {
            if (const auto it = held.applying->keys.find(tagKey); it != held.applying->keys.end()) {
                return it->second.value;
            }
        }
    }
    if (const auto it = held.published.find(tagKey); it != held.published.end()) {
        return it->second.value;
    }
    if (const auto it = held.inForce.find(tagKey); it != held.inForce.end()) {
        return it->second;
    }
    return std::nullopt;
}

// a check may call a design that throws on a wrong value: the throw is the check's refusal, naming no setting, with the
// thrown message, so every path reports it as one the check returned
template<typename TCheck>
[[nodiscard]] std::expected<void, Rejected> refusalOrThrown(TCheck&& check) {
    try {
        return std::forward<TCheck>(check)();
    } catch (const gr::exception& thrown) { // the reason alone, without the place the hook threw it
        return std::unexpected(Rejected{invalidEntry({}, thrown.message)});
    } catch (const std::exception& thrown) {
        return std::unexpected(Rejected{invalidEntry({}, thrown.what())});
    } catch (...) {
        return std::unexpected(Rejected{invalidEntry({}, "exception in checkSettings()")});
    }
}

// each key sent, pending for `cause`
[[nodiscard]] Values pendingFor(const ValueMap& sent, std::string_view cause) {
    Values values;
    for (const auto& [key, _] : sent) {
        values.insert_or_assign(key, Value{.state = Value::State::pending, .value = {}, .reason = std::string(cause)});
    }
    return values;
}

} // namespace

bool sameValue(const pmt::Value& left, const pmt::Value& right) {
    if (left.is_map()) { // member by member, so a not-a-number inside a map compares as one at the top level does
        const pmt::Value::Map* leftMap  = left.get_if<pmt::Value::Map>();
        const pmt::Value::Map* rightMap = right.get_if<pmt::Value::Map>();
        return leftMap != nullptr && rightMap != nullptr && leftMap->size() == rightMap->size() && std::ranges::all_of(*leftMap, [rightMap](const auto& entry) {
            const auto other = rightMap->find(entry.first);
            return other != rightMap->end() && sameValue(entry.second, other->second);
        });
    }
    const auto sameAsRight = [&right]<typename T>(const T& stored) {
        if constexpr (std::same_as<T, std::monostate>) {
            return false;
        } else if constexpr (is_tensor<T>) {
            const T* other = right.get_if<T>();
            if constexpr (std::same_as<typename T::value_type, pmt::Value>) { // a list of values, each compared as a value
                return other != nullptr && std::ranges::equal(stored.extents(), other->extents()) && std::ranges::equal(stored, *other, [](const pmt::Value& one, const pmt::Value& two) { return sameValue(one, two); });
            } else {
                return other != nullptr && std::ranges::equal(stored.extents(), other->extents()) && std::ranges::equal(stored, *other, detail::sameNumber<typename T::value_type>);
            }
        } else {
            const T* other = right.get_if<T>();
            return other != nullptr && detail::sameNumber(stored, *other);
        }
    };
    return left == right || (left.is_tensor() ? visitStoredTensor(left, sameAsRight) : visitStoredScalar(left, sameAsRight));
}

Rejection invalidEntry(std::string key, std::string_view reason) { return Rejection{.key = std::move(key), .reason = std::string(reason)}; }

std::optional<pmt::Value> CallView::state(std::string_view key) const {
    const std::optional<std::string> tagKey = canonicalKey(*_table, key);
    if (!tagKey) {
        return std::nullopt;
    }
    if (const auto it = _converted->find(*tagKey); it != _converted->end()) {
        return it->second;
    }
    return lookUp(*_table, *_held, *tagKey, !_displaced->contains(*tagKey));
}

std::optional<std::string_view> detail::keyOfField(const DeclarationTable& table, void* block, const void* field) {
    for (const auto& [name, declaration] : table.keys) {
        if (declaration.fieldAddress != nullptr && declaration.fieldAddress(block) == field) {
            return name;
        }
    }
    return std::nullopt;
}

namespace {

// the refusal of a port count that would remove a port an edge holds, or none
std::optional<std::string> heldPortRefusal(const GraphLink* link, const Declaration& declaration, std::size_t count) {
    if (link == nullptr) {
        return std::nullopt;
    }
    for (const std::string& collection : declaration.portCollections) {
        if (std::optional<std::string> port = link->edgeAtOrPast(collection, count); port) {
            return std::format("port in use: {}", *port);
        }
    }
    return std::nullopt;
}

Declaration declarationOf(const ReflectedMember& member) {
    Declaration declaration{.name = std::string(member.name), .type = member.type};
    declaration.access          = member.readOnly ? AccessMode::readOnly : AccessMode::readWrite;
    declaration.published       = member.published;
    declaration.reported        = member.role == ReflectedMember::Role::status;
    declaration.fieldAddress    = member.fieldAddress;
    declaration.readField       = member.readField;
    declaration.writeField      = member.writeField;
    declaration.checkFieldLimit = member.checkFieldLimit;
    if (member.annotated) {
        declaration.annotated     = true;
        declaration.documentation = std::string(member.documentation);
        declaration.unit          = std::string(member.unit);
        declaration.visible       = member.visible;
        declaration.stoppedOnly   = member.stoppedOnly;
        for (std::string_view collection : member.portCountFor) {
            declaration.portCollections.emplace_back(collection);
        }
    }
    if (member.typeDetails != nullptr) {
        member.typeDetails(declaration);
    }
    return declaration;
}

} // namespace

template pmt::Value detail::readStoredValue<bool>(const void*);
template pmt::Value detail::readStoredValue<std::int32_t>(const void*);
template pmt::Value detail::readStoredValue<std::uint32_t>(const void*);
template pmt::Value detail::readStoredValue<std::int64_t>(const void*);
template pmt::Value detail::readStoredValue<std::uint64_t>(const void*);
template pmt::Value detail::readStoredValue<float>(const void*);
template pmt::Value detail::readStoredValue<double>(const void*);
template pmt::Value detail::readStoredValue<std::complex<float>>(const void*);
template pmt::Value detail::readStoredValue<std::complex<double>>(const void*);
template pmt::Value detail::readStoredValue<std::string>(const void*);
template pmt::Value detail::readStoredValue<property_map>(const void*);
template pmt::Value detail::readStoredValue<std::vector<float>>(const void*);
template pmt::Value detail::readStoredValue<std::vector<double>>(const void*);
template pmt::Value detail::readStoredValue<std::vector<std::string>>(const void*);
template bool       detail::writeStoredValue<bool>(void*, const pmt::Value&);
template bool       detail::writeStoredValue<std::int32_t>(void*, const pmt::Value&);
template bool       detail::writeStoredValue<std::uint32_t>(void*, const pmt::Value&);
template bool       detail::writeStoredValue<std::int64_t>(void*, const pmt::Value&);
template bool       detail::writeStoredValue<std::uint64_t>(void*, const pmt::Value&);
template bool       detail::writeStoredValue<float>(void*, const pmt::Value&);
template bool       detail::writeStoredValue<double>(void*, const pmt::Value&);
template bool       detail::writeStoredValue<std::complex<float>>(void*, const pmt::Value&);
template bool       detail::writeStoredValue<std::complex<double>>(void*, const pmt::Value&);
template bool       detail::writeStoredValue<std::string>(void*, const pmt::Value&);
template bool       detail::writeStoredValue<property_map>(void*, const pmt::Value&);
template bool       detail::writeStoredValue<std::vector<float>>(void*, const pmt::Value&);
template bool       detail::writeStoredValue<std::vector<double>>(void*, const pmt::Value&);
template bool       detail::writeStoredValue<std::vector<std::string>>(void*, const pmt::Value&);

DeclarationTable detail::makeDeclarationTable(std::string blockType, std::span<const ReflectedMember> members, const property_map* description, bool isScheduler) {
    DeclarationTable table{.blockType = std::move(blockType), .isScheduler = isScheduler};
    for (const ReflectedMember& member : members) {
        if (member.role == ReflectedMember::Role::portCollection) {
            table.portCollections.emplace_back(member.name);
            if (member.resizeCollection != nullptr) {
                table.collectionResizes.emplace(member.name, member.resizeCollection);
            }
            if (member.sizeOfCollection != nullptr) {
                table.collectionSizes.emplace(member.name, member.sizeOfCollection);
            }
        } else if (member.role != ReflectedMember::Role::other) {
            Declaration declaration = declarationOf(member);
            table.keys.emplace(declaration.name, std::move(declaration));
        }
    }
    if (description != nullptr) {
        addDescription(table, *description);
    }
    finishDeclarationTable(table, isScheduler);
    return table;
}

bool CallView::changed(std::string_view key) const {
    const std::optional<std::string> tagKey = canonicalKey(*_table, key);
    if (!tagKey) {
        return false;
    }
    if (_wholeState) {
        return state(*tagKey).has_value();
    }
    const auto it = _converted->find(*tagKey);
    if (it == _converted->end()) {
        return false;
    }
    const std::optional<pmt::Value> current = lookUp(*_table, *_held, *tagKey, !_displaced->contains(*tagKey));
    return !current || !sameValue(*current, it->second);
}

detail::OwnThread::OwnThread(const SettingsBase& blockSettings) noexcept : settings(&blockSettings), outer(ownThreadMarks()) { ownThreadMarks() = this; }

} // namespace gr::settings

namespace gr {

using namespace gr::settings;
using namespace gr::settings::detail;

bool SettingsBase::onOwnThread() const noexcept {
    for (const settings::detail::OwnThread* mark = settings::detail::ownThreadMarks(); mark != nullptr; mark = mark->outer) {
        if (mark->settings == this) {
            return true;
        }
    }
    return false;
}

// only the holder writes its own id, and it writes another before it lets the lock go, so a relaxed load suffices
bool SettingsBase::holdsLock() const noexcept { return _lockHolder.load(std::memory_order_relaxed) == std::this_thread::get_id(); }

void SettingsBase::BlockLock::lock() { std::ignore = lockUntil(std::nullopt); }

bool SettingsBase::BlockLock::lockUntil(std::optional<std::chrono::steady_clock::time_point> deadline) {
    if (_owns || _settings.holdsLock()) {
        return true;
    }
    if (!deadline) {
        _settings._lock.lock();
    } else if (!_settings._lock.try_lock_until(*deadline)) {
        return false;
    }
    _settings._lockHolder.store(std::this_thread::get_id(), std::memory_order_relaxed);
    _owns = true;
    return true;
}

void SettingsBase::BlockLock::unlock() {
    if (!_owns) {
        return;
    }
    _settings._lockHolder.store(std::thread::id{}, std::memory_order_relaxed);
    _settings._lock.unlock();
    _owns = false;
}

std::vector<std::pair<std::string_view, std::size_t>> SettingsBase::unsizedCollectionSizes() const {
    std::vector<std::pair<std::string_view, std::size_t>> sizes;
    for (const auto& [collection, sizeOf] : _table.collectionSizes) {
        const bool sized = std::ranges::any_of(_table.keys, [&collection](const auto& entry) { return std::ranges::contains(entry.second.portCollections, collection); });
        if (!sized && _table.collectionResizes.contains(collection)) {
            sizes.emplace_back(collection, sizeOf(_block));
        }
    }
    return sizes;
}

void SettingsBase::restoreUnpublishedFields(const ValueMap& fields) {
    for (const auto& [name, declaration] : _table.keys) {
        if (declaration.source != DeclarationSource::reflected || declaration.writeField == nullptr || declaration.published || _toWrite.contains(name)) {
            continue;
        }
        const auto inForce = _inForce.find(name);
        const auto field   = fields.find(name);
        if (inForce != _inForce.end() && field != fields.end() && !sameValue(inForce->second, field->second)) {
            notify(NotificationLevel::error, NotificationKind::failed, name, kWrittenOutsideApply);
            std::ignore = writeField(*_table.find(name), name, inForce->second);
        }
    }
}

SettingsBase::SettingsBase(const DeclarationTable& table, void* block, property_map* described, property_map constructedWith) : _table(table), _block(block), _described(described), _constructedWith(std::move(constructedWith)) {
    for (const auto& [name, declaration] : _table.keys) {
        if (declaration.source == DeclarationSource::core) {
            _coreFields.insert_or_assign(name, declaration.defaultValue);
        }
    }
}

SettingsBase::SettingsBase(SettingsBase&& other, void* block, property_map* described) noexcept : _table(other._table), _block(block), _described(described) {
    std::lock_guard lock(other._lock);
    _constructedWith = std::move(other._constructedWith);
    _coreFields      = std::move(other._coreFields);
    _inForce         = std::exchange(other._inForce, ValueMap{});
    _toWrite         = std::move(other._toWrite);
    _pending         = std::move(other._pending);
    {
        std::lock_guard slotLock(other._slotLock);
        _slot = std::move(other._slot);
    }
    _applying      = std::move(other._applying);
    _record        = std::move(other._record);
    _version       = other._version;
    _nextBatch     = other._nextBatch;
    _applyingBatch = other._applyingBatch;
    _appliedBatch  = other._appliedBatch;
    _failures      = std::move(other._failures);
    _published     = std::move(other._published);
    _couplings     = std::move(other._couplings);
    _ownLevel.store(other._ownLevel.load(std::memory_order_relaxed), std::memory_order_relaxed);
    _state          = other._state;
    _starting       = other._starting;
    _storedDefaults = std::move(other._storedDefaults);
    _setUp          = other._setUp;
    _notReady       = std::move(other._notReady);
    _graphLink      = std::exchange(other._graphLink, nullptr);
    _schedulerLink.store(other._schedulerLink.exchange(nullptr), std::memory_order_release);
    _run = std::move(other._run);
    _hasWork.store(other._hasWork.load(std::memory_order_relaxed), std::memory_order_relaxed);
    _toForward = std::move(other._toForward);
    _composite = std::exchange(other._composite, nullptr);
    if (_composite != nullptr) {
        _composite->attachOwner(*this);
    }
    _compositeEntry.store(other._compositeEntry.exchange(nullptr));
    _innerTagKeys = std::move(other._innerTagKeys);
    _nTagReasons.store(other._nTagReasons.exchange(0UZ));
}

std::optional<pmt::Value> SettingsBase::readField(const KeyRef& ref, std::string_view tagKey) const {
    const Declaration& declaration = *ref.declaration;
    if (declaration.readField != nullptr) {
        return declaration.readField(_block);
    }
    const auto readFrom = [tagKey](const auto& map) -> std::optional<pmt::Value> {
        const auto it = map.find(tagKey);
        return it != map.end() ? std::optional<pmt::Value>(it->second) : std::nullopt;
    };
    if (declaration.source == DeclarationSource::core) {
        return readFrom(_coreFields);
    }
    return _described != nullptr ? readFrom(*_described) : std::nullopt;
}

bool SettingsBase::writeField(const KeyRef& ref, std::string_view tagKey, const pmt::Value& value) {
    const Declaration& declaration = *ref.declaration;
    if (declaration.writeField != nullptr) {
        return declaration.writeField(_block, value);
    }
    if (declaration.source == DeclarationSource::core) {
        assignAt(_coreFields, tagKey, value);
        return true;
    }
    if (declaration.source == DeclarationSource::described && _described != nullptr) {
        _described->insert_or_assign(std::pmr::string(tagKey), value);
        return true;
    }
    return false;
}

void SettingsBase::eraseField(const KeyRef& ref, std::string_view tagKey) {
    if (ref.declaration->source == DeclarationSource::core) {
        if (const auto it = _coreFields.find(tagKey); it != _coreFields.end()) {
            _coreFields.erase(it);
        }
    } else if (ref.declaration->source == DeclarationSource::described && _described != nullptr) {
        if (const auto it = _described->find(tagKey); it != _described->end()) {
            _described->erase(it);
        }
    }
}

ValueMap SettingsBase::readFields(bool published) const {
    ValueMap fields;
    for (const auto& [name, declaration] : _table.keys) {
        if (declaration.readField != nullptr && declaration.published == published) {
            fields.insert_or_assign(name, declaration.readField(_block));
        }
    }
    if (!published) {
        fields.insert(_coreFields.begin(), _coreFields.end());
    }
    if (_described != nullptr) {
        for (const auto& [key, value] : *_described) {
            if (const std::optional<KeyRef> ref = _table.find(key); ref && !ref->bareName && ref->declaration->published == published) {
                fields.insert_or_assign(std::string(key), value);
            }
        }
    }
    return fields;
}

// the reads of readFields(false) without building its map, and each published key's value last set counted as read:
// a key it reads twice, or a key `values` lacks, holds nothing
bool SettingsBase::fieldsHold(const ValueMap& values) const {
    std::size_t nRead = 0UZ;
    const auto  holds = [&values, &nRead](std::string_view key, const pmt::Value& value) {
        ++nRead;
        const auto it = values.find(key);
        return it != values.end() && sameValue(it->second, value);
    };
    for (const auto& [name, declaration] : _table.keys) {
        if (declaration.published) {
            nRead += values.contains(name) ? 1UZ : 0UZ;
        } else if (declaration.readField != nullptr && !holds(name, declaration.readField(_block))) {
            return false;
        }
    }
    for (const auto& [key, value] : _coreFields) {
        if (!holds(key, value)) {
            return false;
        }
    }
    if (_described != nullptr) {
        for (const auto& [key, value] : *_described) {
            if (const std::optional<KeyRef> ref = _table.find(key); ref && !ref->bareName && !ref->declaration->published && !holds(key, value)) {
                return false;
            }
        }
    }
    return nRead == values.size();
}

void SettingsBase::rereadField(ValueMap& values, std::string_view key) const {
    const std::optional<KeyRef>     ref   = _table.find(key);
    const std::optional<pmt::Value> value = ref ? readField(*ref, key) : std::nullopt;
    if (const auto it = values.find(key); it != values.end()) {
        if (value) {
            it->second = *value;
        } else {
            values.erase(it);
        }
    } else if (value) {
        values.emplace(std::string(key), *value);
    }
}

ValueMap SettingsBase::sourceBeforeInit() const {
    ValueMap source = readFields(false);
    source.merge(readFields(true));
    for (const auto& [key, sent] : _constructedWith) {
        const std::optional<KeyRef> ref = _table.find(key);
        if (!ref || ref->declaration->isReadOnly()) {
            continue;
        }
        const std::string                      tagKey    = tagKeyOf(*ref);
        std::expected<pmt::Value, std::string> converted = convert(*ref->declaration, sent);
        if (converted) {
            source.insert_or_assign(tagKey, std::move(*converted));
        }
    }
    for (const auto& [name, declaration] : _table.keys) {
        if (declaration.source == DeclarationSource::described && !declaration.isReadOnly() && declaration.defaultValue.has_value()) {
            source.try_emplace(name, declaration.defaultValue);
        }
    }
    for (const auto& [groupName, group] : _table.groups) {
        if (group.countKey.empty()) {
            continue;
        }
        const auto          countIt = source.find(group.countKey);
        const std::uint64_t count   = countFrom(countIt != source.end() ? &countIt->second : nullptr);
        for (std::uint64_t member = 0U; member < count; ++member) {
            for (const auto& [name, declaration] : group.settings) {
                if (declaration.defaultValue.has_value()) {
                    source.try_emplace(std::format("{}.{}.{}", groupName, member, name), declaration.defaultValue);
                }
            }
        }
    }
    return source;
}

void SettingsBase::eraseMembersPastCount(const KeyRef& countRef) {
    const std::optional<pmt::Value> countValue = readField(countRef, countRef.declaration->name);
    const std::uint64_t             count      = countFrom(countValue ? &*countValue : nullptr);
    forEachCountedGroup(_table, countRef.declaration->name, [&](const MemberGroup& group) {
        const auto past = [&](std::string_view key) { return isMemberPast(_table, key, group, count); };
        if (_described != nullptr) {
            std::erase_if(*_described, [&](const auto& entry) { return past(entry.first); });
        }
        std::erase_if(_published, [&](const auto& entry) { return past(entry.first); });
    });
}

KeySet SettingsBase::writePublished() {
    KeySet written;
    for (const auto& [key, value] : _toWrite) {
        const std::optional<KeyRef> ref = _table.find(key);
        if (!ref) {
            continue;
        }
        if (value.has_value()) {
            std::ignore = writeField(*ref, key, value);
        } else {
            eraseField(*ref, key);
        }
        written.insert(key);
    }
    _toWrite.clear();
    return written;
}

bool SettingsBase::isPublishedKey(std::string_view key) const {
    const std::optional<KeyRef> ref = _table.find(key);
    return ref && ref->declaration->published;
}

std::optional<pmt::Value> SettingsBase::heldValue(std::string_view key) const {
    const std::optional<KeyRef> ref = _table.find(key);
    if (ref && ref->declaration->published && !_fieldsEnded) {
        if (const auto pending = _toWrite.find(key); pending != _toWrite.end()) {
            return pending->second.has_value() ? std::optional(pending->second) : std::nullopt;
        }
        return readField(*ref, key);
    }
    if (const auto published = _published.find(key); published != _published.end()) {
        return published->second.value;
    }
    const auto inForce = _inForce.find(key);
    return inForce != _inForce.end() ? std::optional(inForce->second) : std::nullopt;
}

// a write is a change when it differs from the value a read returns, which is the value the block published by key in
// place of the value last set where it published one
bool SettingsBase::holdsInForce(std::string_view tagKey, const pmt::Value& value) const {
    const std::optional<pmt::Value> held = heldValue(tagKey);
    return held && sameValue(*held, value);
}

void SettingsBase::storeDefaultsFrom(const ValueMap& values) {
    _storedDefaults.clear();
    for (const auto& [key, value] : values) {
        const std::optional<KeyRef> ref = _table.find(key);
        // a value the block derived or a device published for a key no caller gave would come back given from a reset
        if (ref && !ref->declaration->isReadOnly() && (hasDefault(*ref->declaration) || _record.contains(key))) {
            _storedDefaults.insert_or_assign(key, value);
        }
    }
}

void SettingsBase::storeInForceAsDefaults() {
    if (!firstUse()) { // a block that is not ready holds no values to keep
        return;
    }
    const BlockLock lock(*this);
    storeDefaultsFrom(_inForce);
}

property_map SettingsBase::pendingValues() const {
    const BlockLock lock(*this);
    property_map    pending;
    for (const auto& [key, change] : _pending.keys) {
        pending.insert_or_assign(std::pmr::string(key), change.value);
    }
    return pending;
}

std::string SettingsBase::tagKeyOrSent(std::string_view key) const { return canonicalKey(_table, key).value_or(std::string(key)); }

std::optional<SettingsBase::Threshold> SettingsBase::threshold() const {
    const std::uint8_t own = _ownLevel.load(std::memory_order_relaxed);
    if (own == static_cast<std::uint8_t>(NotificationLevel::off)) {
        return std::nullopt;
    }
    if (own != kNoLevel) {
        return Threshold{.level = static_cast<NotificationLevel>(own)};
    }
    const SchedulerLink*                   link      = _schedulerLink.load(std::memory_order_acquire);
    const std::optional<NotificationLevel> inherited = link != nullptr ? link->level() : std::nullopt;
    if (inherited == NotificationLevel::off) {
        return Threshold{.level = NotificationLevel::warning, .quiet = true};
    }
    // a block with no scheduler inherits no level and uses warning, a top-level scheduler's default
    return Threshold{.level = inherited.value_or(NotificationLevel::warning)};
}

bool SettingsBase::passes(NotificationLevel level) const {
    const std::optional<Threshold> limit = threshold();
    return limit && level >= std::min(limit->level, NotificationLevel::error);
}

void SettingsBase::notify(NotificationLevel level, NotificationKind what, std::string_view key, std::string_view reason) const {
    const std::optional<Threshold> limit = threshold();
    if (!limit || level < std::min(limit->level, NotificationLevel::error)) {
        return;
    }
    Notification notification{.level = level, .what = what, .block = uniqueName(), .key = std::string(key), .reason = std::string(reason), .quiet = limit->quiet};
    if (!key.empty() && !_table.find(key)) {
        notification.key    = {};
        notification.reason = std::format("{}: {}", key, reason);
    }
    SchedulerLink* link = _schedulerLink.load(std::memory_order_acquire);
    if (link == nullptr) { // a block with no scheduler has no queue, so the default reader prints on this thread
        printNotification(notification);
        return;
    }
    link->push(std::move(notification));
    link->wake();
}

std::expected<void, Rejected> SettingsBase::init() {
    const BlockLock lock(*this);        // the setup in one hold, so a call on another thread waits for its end
    if (_setUp || !_notReady.empty()) { // a block that is not ready waits for a `set`, which judges it again
        return {};
    }
    if (onOwnThread()) { // a hook that `init()` runs made a settings call on its own block
        return {};
    }
    const settings::detail::OwnThread applying(*this);
    SetUpVerdict                      verdict = setUpWith({}, true);
    if (verdict.entries.empty()) {
        return {};
    }
    if (!verdict.notReady) {
        return std::unexpected(std::move(verdict.entries));
    }
    _notReady = std::move(verdict.entries);
    return {};
}

SetUpVerdict SettingsBase::setUpWith(const property_map& changes, bool store) {
    assert(holdsLock() && "the setup runs in one hold of the block lock");
    describeBlock();
    if (!_table.faults.empty()) {
        return SetUpVerdict{.entries = _table.faults};
    }
    _inForce                = readFields(false);
    const ValueMap declared = _inForce;
    property_map   given    = _constructedWith;
    for (const auto& [key, value] : changes) {
        std::erase_if(given, [&](const auto& entry) { return tagKeyOrSent(entry.first) == tagKeyOrSent(key); });
        given.insert_or_assign(key, value);
    }
    KeySet givenKeys;
    for (const auto& [key, _] : given) {
        givenKeys.insert(tagKeyOrSent(key));
    }
    Rejected notReady = refusedDefaults(givenKeys);
    // a field that no caller gave and that differs from the value a default-constructed `Settings` holds was assigned
    // before the setup, from outside the block or by its constructor: the value would bypass the check and the record
    KeySet assigned;
    for (const auto& [key, constructed] : structDefaults()) {
        const auto field = declared.find(key);
        if (!givenKeys.contains(key) && field != declared.end() && !sameValue(field->second, constructed)) {
            assigned.insert(key);
        }
    }
    std::erase_if(notReady, [&assigned](const Rejection& entry) { return assigned.contains(entry.key); });
    for (const std::string& key : assigned) {
        notReady.push_back(invalidEntry(key, std::string(kAssignedDirectly)));
    }
    // the check of the whole state runs whether or not the block was built with values
    std::expected<CheckedCall, Rejected> checked = checkChanges(given);
    if (!checked) {
        // the values given are at fault unless the declared defaults alone are refused for the same reasons
        if (const std::expected<CheckedCall, Rejected> defaultsAlone = checkChanges({}); defaultsAlone || defaultsAlone.error() != checked.error()) {
            return SetUpVerdict{.entries = std::move(checked.error())};
        }
        notReady.insert(notReady.end(), checked.error().begin(), checked.error().end());
    }
    if (!notReady.empty()) {
        return SetUpVerdict{.entries = std::move(notReady), .notReady = true};
    }
    if (!store) {
        return {};
    }
    Merged merged = merge(*checked, false, false);
    if (Rejected failed = apply(); !failed.empty()) {
        return SetUpVerdict{.entries = std::move(failed)};
    }
    copyStatus(); // a read finds each status value from the setup on
    _setUp = true;
    _notReady.clear();
    // the apply ran before the block was set up and left the flag raised; only a change made since keeps it
    storeHasWork(!_pending.empty() || !_toWrite.empty());
    // the apply above left in force the fields it read, and nothing has run since. A reset returns to the values the
    // block was built with, so a call's value that made the block ready is no default
    ValueMap defaults = _inForce;
    for (const auto& [key, _] : changes) {
        const std::string tagKey = tagKeyOrSent(key);
        if (const auto it = declared.find(tagKey); it != declared.end() && std::ranges::none_of(_constructedWith, [&](const auto& entry) { return tagKeyOrSent(entry.first) == tagKey; })) {
            defaults.insert_or_assign(tagKey, it->second);
        }
    }
    storeDefaultsFrom(defaults);
    return SetUpVerdict{.merged = std::move(merged)};
}

std::expected<void, Rejected> SettingsBase::firstUse() {
    const auto heldBack = [this]() -> std::expected<void, Rejected> {
        const BlockLock lock(*this);
        if (!_notReady.empty()) {
            return std::unexpected(_notReady);
        }
        return {};
    };
    {
        const BlockLock lock(*this);
        if (_setUp) {
            return {};
        }
    }
    return init().and_then(heldBack);
}

std::expected<void, Rejected> SettingsBase::ready() { return firstUse(); }

bool SettingsBase::heldNotReady() const {
    const BlockLock lock(*this);
    return !_notReady.empty();
}

Rejected SettingsBase::refusedDefaults(const KeySet& given) const {
    Rejected        refused;
    const BlockLock lock(*this);
    for (const auto& [name, declaration] : _table.keys) {
        const auto inForce = _inForce.find(name);
        if (declaration.source != DeclarationSource::reflected || declaration.isReadOnly() || given.contains(name) || inForce == _inForce.end()) {
            continue;
        }
        if (const std::optional<std::string> refusal = refuseByLimits(declaration, inForce->second); refusal) {
            refused.push_back(invalidEntry(name, std::format("default {}", *refusal)));
        }
    }
    return refused;
}

std::expected<void, Rejected> SettingsBase::runCheck(const CallView& view) const {
    struct CallProposal final : settings::detail::Proposal {
        const CallView& view;

        explicit CallProposal(const CallView& callView) : view(callView) {}

        std::optional<pmt::Value> after(std::string_view tagKey) const override { return view.state(tagKey); }
    };
    const CallProposal           proposal(view);
    const std::optional<Refused> refused = runCheckHook(proposal);
    if (!refused || refused->entries.empty()) {
        return {};
    }
    Rejected entries;
    for (const Refused::Entry& entry : refused->entries) {
        const std::optional<std::string_view> member = entry.field != nullptr ? keyOfField(_table, _block, entry.field) : std::nullopt;
        assert((member || entry.field == nullptr || !entry.key.empty()) && "a refusal names a setting member of its own block");
        entries.push_back(invalidEntry(member ? std::string(*member) : (entry.key.empty() ? std::string{} : tagKeyOrSent(entry.key)), entry.reason));
    }
    return std::unexpected(std::move(entries));
}

std::expected<CheckedCall, Rejected> SettingsBase::checkChanges(const property_map& changes) {
    assert(holdsLock() && "the caller holds the block lock through the check and its merge");
    const CheckState held{.pending = _pending, .applying = _applying.get(), .inForce = _inForce, .published = _published, .graphLink = _graphLink, .state = _state, .starting = _starting, .setUp = _setUp};
    CheckedCall      checked{.stateNoted = held.state};
    Rejected         entries;
    // a scheduler set up at its start has not run yet: its setup judges each value alone, never as a change while running
    const bool runsOrPaused = held.setUp && (held.state == BlockState::running || held.state == BlockState::paused || held.starting);
    // a value no state takes, sent to a key that waits for a stop while the block runs, is refused for the stop and for
    // the value at once, so that one change of the call and one stop serve the caller; an unchanged key waits for nothing
    const auto refuseValue = [&](const std::string& tagKey, const Declaration& declaration, const pmt::Value* converted, std::string reason) {
        const auto inForce   = held.inForce.find(tagKey);
        const bool unchanged = converted != nullptr && inForce != held.inForce.end() && sameValue(inForce->second, *converted);
        if (runsOrPaused && (declaration.isPortKey() || declaration.atStart()) && !unchanged) {
            entries.push_back(invalidEntry(tagKey, kWhileStopped));
        }
        entries.push_back(invalidEntry(tagKey, reason));
    };
    for (const auto& [sentKey, sent] : changes) {
        const std::optional<KeyRef> ref = _table.find(sentKey);
        if (!ref) {
            entries.push_back(invalidEntry(std::string(sentKey), kUndeclared));
            continue;
        }
        const std::string tagKey = tagKeyOf(*ref);
        if (ref->declaration->isReadOnly()) {
            entries.push_back(invalidEntry(tagKey, ref->declaration->reported ? kStatus : kReadOnly));
            continue;
        }
        std::expected<pmt::Value, std::string> converted = convert(*ref->declaration, sent);
        if (!converted) {
            refuseValue(tagKey, *ref->declaration, nullptr, std::move(converted.error()));
            continue;
        }
        if (const auto it = checked.converted.find(tagKey); it != checked.converted.end()) {
            if (!sameValue(it->second, *converted)) {
                entries.push_back(invalidEntry(tagKey, "two values in one call"));
            }
            continue;
        }
        // a value a caller gives meets the limits even when it equals the one in force, which may be a default outside them
        if (std::optional<std::string> refusal = refuseByLimits(*ref->declaration, *converted); refusal) {
            refuseValue(tagKey, *ref->declaration, &*converted, std::move(*refusal));
            continue;
        }
        checked.converted.emplace(tagKey, std::move(*converted));
    }

    const auto countLeft = [&](const std::string& countKey) {
        if (const auto it = checked.converted.find(countKey); it != checked.converted.end()) {
            return countFrom(&it->second);
        }
        const std::optional<pmt::Value> current = lookUp(_table, held, countKey, true);
        return countFrom(current ? &*current : nullptr);
    };
    std::erase_if(checked.converted, [&](const auto& entry) {
        const std::optional<KeyRef> ref = _table.find(entry.first);
        if (ref->group == nullptr || ref->group->countKey.empty() || ref->member < countLeft(ref->group->countKey)) {
            return false;
        }
        entries.push_back(invalidEntry(entry.first, kUndeclared));
        return true;
    });

    for (const auto& [key, value] : checked.converted) {
        const std::uint64_t count = countFrom(&value);
        forEachCountedGroup(_table, key, [&](const MemberGroup& group) {
            for (const auto& [pendingKey, _] : held.pending.keys) {
                if (isMemberPast(_table, pendingKey, group, count)) {
                    checked.displaced.insert(pendingKey);
                }
            }
        });
    }
    for (const auto& [key, _] : checked.converted) {
        checked.given.insert(key);
    }

    std::erase_if(checked.converted, [&](const auto& entry) {
        const auto& [key, value]       = entry;
        const Declaration& declaration = *_table.find(key)->declaration;
        if (!declaration.isPortKey() && !declaration.atStart()) {
            return false;
        }
        const auto inForce = held.inForce.find(key);
        if (inForce != held.inForce.end() && sameValue(inForce->second, value) && (declaration.isPortKey() || runsOrPaused)) {
            checked.given.erase(key);
            return true;
        }
        if (runsOrPaused) {
            entries.push_back(invalidEntry(key, kWhileStopped));
            return false;
        }
        if (declaration.atStart()) {
            return false;
        }
        const std::optional<std::uint64_t> count = settings::detail::losslessAs<std::uint64_t>(value);
        if (!count) {
            entries.push_back(invalidEntry(key, "not a whole number >= 0"));
            return false;
        }
        if (!std::in_range<std::size_t>(*count)) {
            entries.push_back(invalidEntry(key, std::format("out of range [0, {}]: {}", std::numeric_limits<std::size_t>::max(), *count)));
            return false;
        }
        if (std::optional<std::string> inUse = heldPortRefusal(held.graphLink, declaration, static_cast<std::size_t>(*count)); inUse) {
            entries.push_back(invalidEntry(key, *inUse));
            return false;
        }
        checked.changesPorts = true;
        return false;
    });
    // a call refused only for keys that wait for a stop is still judged by the check hook, so that the one refusal also
    // gives a reason the value has in any state
    const bool onlyWhileStopped = std::ranges::all_of(entries, [](const Rejection& entry) { return entry.reason == kWhileStopped; });
    if (!entries.empty() && !onlyWhileStopped) {
        return std::unexpected(std::move(entries));
    }

    if (hasCheckHook()) {
        const CallView view(_table, checked.converted, checked.displaced, held);
        if (view._wholeState || std::ranges::any_of(checked.converted, [&view](const auto& entry) { return view.changed(entry.first); })) {
            // a refusal that names no key is reported against each key the call changes; the whole state's stays against the block
            const auto againstChanged = [&](Rejected refused) {
                if (view._wholeState) {
                    return refused;
                }
                Rejected spread;
                for (Rejection& entry : refused) {
                    if (!entry.key.empty()) {
                        spread.push_back(std::move(entry));
                        continue;
                    }
                    for (const auto& [key, _] : checked.converted) {
                        if (view.changed(key)) {
                            spread.push_back(invalidEntry(key, entry.reason));
                        }
                    }
                }
                return spread;
            };
            std::expected<void, Rejected> verdict = refusalOrThrown([&] { return runCheck(view); }).transform_error(againstChanged);
            if (!verdict) {
                std::ranges::move(verdict.error(), std::back_inserter(entries));
                return std::unexpected(std::move(entries));
            }
        }
    }
    if (!entries.empty()) {
        return std::unexpected(std::move(entries));
    }
    return checked;
}

Merged SettingsBase::merge(const CheckedCall& checked, bool fromTag, bool reset) {
    assert(holdsLock() && "the merge runs in the hold of its check");
    const bool waking = !fromTag && !checked.converted.empty() && _state == BlockState::running;
    for (const std::string& key : checked.displaced) {
        _pending.keys.erase(key);
    }
    for (const auto& [key, value] : checked.converted) {
        _pending.keys[key].value = value;
    }
    for (const std::string& key : checked.given) {
        _pending.keys[key].given = true;
    }
    if (reset) {
        _pending.reset = true;
    }
    if (!checked.converted.empty() || !checked.displaced.empty() || reset) {
        ++_version;
    }
    if (!checked.converted.empty() || reset) {
        _hasWork.store(true, std::memory_order_release);
    }
    return Merged{.sent = checked.converted, .batch = _nextBatch, .graphLink = _graphLink, .schedulerLink = _schedulerLink.load(std::memory_order_acquire), .stateNoted = checked.stateNoted, .waking = waking, .changesPorts = checked.changesPorts};
}

std::expected<Merged, Rejected> SettingsBase::checkAndMerge(const property_map& changes, const KeySet* tagSets, bool reset) {
    const BlockLock lock(*this);
    drainSlot(); // the slot's keys came first
    std::expected<CheckedCall, Rejected> checked = checkChanges(changes);
    if (!checked) {
        return std::unexpected(std::move(checked.error()));
    }
    if (tagSets != nullptr) { // a tag's value follows the stream and makes no key given; a reset tag's defaults are no key it sets
        std::erase_if(checked->given, [tagSets](const std::string& key) { return tagSets->contains(key); });
    }
    return merge(*checked, tagSets != nullptr, reset);
}

bool SettingsBase::applyAtCall(const Merged& merged) {
    // no work call reaches a subgraph, so its own keys apply at the call whatever its state
    if (!merged.changesPorts && !(merged.graphLink == nullptr && merged.stateNoted != BlockState::running) && !isSubgraph()) {
        return false;
    }
    std::ignore = apply();
    if (merged.changesPorts) {
        if (merged.graphLink != nullptr) {
            merged.graphLink->refreshPorts();
        }
        const BlockLock lock(*this);
        ++_version;
    }
    return true;
}

Rejected SettingsBase::apply() {
    const settings::detail::OwnThread applying(*this);
    BlockLock                         lock(*this); // held across the hooks, which call back into these settings under it
    drainSlot();
    KeySet publishedKeys = writePublished();
    if (_pending.empty() && _setUp) {
        storeHasWork(false);
        return {};
    }
    const std::uint64_t batch = _nextBatch++;
    _applyingBatch            = batch;
    const bool atInit         = !_setUp;
    // the fields hold the values in force unless the block wrote one outside its apply hook; only then are they read whole
    std::optional<ValueMap> readBefore;
    if (atInit || !fieldsHold(_inForce)) {
        if (!atInit) {
            restoreUnpublishedFields(readFields(false));
        }
        readBefore = readFields(false);
    }
    const ValueMap& oldFields    = readBefore ? *readBefore : _inForce; // in force stays as it was until the batch has written
    const auto      oldPublished = _published;
    const auto      taken        = std::make_shared<const PendingChanges>(std::exchange(_pending, {}));
    _applying                    = taken;

    std::map<std::string, std::string, std::less<>> failed;
    KeySet                                          written;
    for (const auto& [key, change] : taken->keys) {
        const std::optional<KeyRef> ref = _table.find(key);
        if (ref && writeField(*ref, key, change.value)) {
            written.insert(key);
            _published.erase(key);
        } else {
            failed.insert_or_assign(key, std::format("field type mismatch: {}", change.value));
        }
    }
    for (const std::string& key : written) {
        const auto oldCount = oldFields.find(key);
        const auto newCount = readField(*_table.find(key), key);
        if (!newCount || countFrom(&*newCount) <= countFrom(oldCount != oldFields.end() ? &oldCount->second : nullptr) || _described == nullptr) {
            continue;
        }
        forEachCountedGroup(_table, key, [&](const MemberGroup& group) {
            for (std::uint64_t member = 0U; member < countFrom(&*newCount); ++member) {
                for (const auto& [name, declaration] : group.settings) {
                    if (declaration.defaultValue.has_value()) {
                        _described->try_emplace(std::pmr::string(std::format("{}.{}.{}", group.name, member, name)), declaration.defaultValue);
                    }
                }
            }
        });
    }

    // at the initialization every setting is written and changed, with no value before
    if (atInit) {
        for (const auto& [key, value] : *readBefore) {
            if (const std::optional<KeyRef> ref = _table.find(key); ref && !ref->declaration->isReadOnly() && !failed.contains(key)) {
                written.insert(key);
            }
        }
    }
    property_map changedValues;
    ValueMap     previousValues;
    for (const std::string& key : written) {
        const std::optional<KeyRef>     ref = _table.find(key);
        const std::optional<pmt::Value> now = readField(*ref, key);
        // the value before is the one a read returned: a value the block published by key in place of the value last set,
        // else the field's
        const pmt::Value* old = nullptr;
        if (const auto published = oldPublished.find(key); published != oldPublished.end()) {
            old = &published->second.value;
        } else if (const auto field = oldFields.find(key); field != oldFields.end()) {
            old = &field->second;
        }
        if (old != nullptr && !atInit) {
            previousValues.insert_or_assign(key, *old);
        }
        if (now && (atInit || old == nullptr || !sameValue(*old, *now))) {
            changedValues.insert_or_assign(std::pmr::string(key), *now);
        }
    }

    property_map                                    forwards;
    std::map<std::string, std::string, std::less<>> hookFailures;
    std::optional<std::string>                      thrown;
    std::expected<void, Rejected>                   verdict;
    const bool                                      hookRan      = !changedValues.empty() && hasApplyHook();
    bool                                            portsResized = false;
    if (hookRan) {
        const std::vector<std::pair<std::string_view, std::size_t>> unsizedBefore = unsizedCollectionSizes();
        _forwarding                                                               = &forwards;
        _hookFailures                                                             = &hookFailures;
        try {
            if (applyTakesSettings()) {
                runSettingsApplyHook(changedValues, previousValues, atInit ? nullptr : &oldFields);
            } else {
                verdict = runMembersApply(changedValues);
            }
        } catch (const gr::exception& exception) {
            thrown = exception.message;
        } catch (const std::exception& exception) {
            thrown = exception.what();
        } catch (...) {
            thrown = "exception in applySettings()";
        }
        copyStatus();
        _forwarding   = nullptr;
        _hookFailures = nullptr;
        // the block's port list follows a collection only where a port key sizes it, so the hook's resize of any other
        // collection fails the batch, and the collection takes back its size. A growth may have moved the ports, whose
        // addresses the port list holds: the graph lists them again
        for (const auto& [collection, size] : unsizedBefore) {
            if (_table.collectionSizes.find(collection)->second(_block) != size) {
                _table.collectionResizes.find(collection)->second(_block, size);
                thrown       = thrown.value_or(std::format("port collection '{}': resized by applySettings() without PortCountFor", collection));
                portsResized = true;
            }
        }
    }
    // The apply hook may assign one of the block's settings, as a command it takes back or a value it rounds to what its
    // device has. A host reads and saves the assigned value, and the next apply's `old` holds it. An assigned value meets
    // the key's limits, as a caller's does; one outside them is a failure of the hook.
    KeySet assigned;
    if (hookRan && applyTakesSettings()) {
        for (const auto& [key, now] : readFields(false)) {
            const std::optional<KeyRef> ref = _table.find(key);
            if (!ref || ref->declaration->source != DeclarationSource::reflected || ref->declaration->published) {
                continue;
            }
            const pmt::Value* beforeHook = nullptr; // a key the batch wrote holds the value it wrote, any other its value before
            if (const auto wrote = changedValues.find(key); wrote != changedValues.end()) {
                beforeHook = &wrote->second;
            } else if (const auto previous = previousValues.find(key); previous != previousValues.end()) {
                beforeHook = &previous->second;
            } else if (const auto other = oldFields.find(key); other != oldFields.end()) {
                beforeHook = &other->second;
            }
            if (beforeHook == nullptr || sameValue(*beforeHook, now)) {
                continue;
            }
            assigned.insert(key);
            if (std::optional<std::string> refusal = refuseByLimits(*ref->declaration, now); refusal && !thrown) {
                hookFailures.try_emplace(key, std::format("{} (assigned by the block)", *refusal));
            }
        }
    }
    // the hook is not run again, so a failure in it takes back every key the batch changed and every setting it assigned:
    // a key kept in force could break a rule it holds with a key taken back. The key the block failed or assigned keeps
    // its reason and each other key names it
    KeySet     takenBack;
    KeySet     namesOther; // the failed keys whose reason names the key at fault
    const bool batchFails = thrown.has_value() || !hookFailures.empty();
    if (batchFails) {
        for (const auto& [changedKey, _] : changedValues) {
            const std::string key(changedKey);
            takenBack.insert(key);
            if (thrown) {
                failed.insert_or_assign(key, *thrown);
            } else if (const auto own = hookFailures.find(key); own != hookFailures.end()) {
                failed.insert_or_assign(key, own->second);
            } else {
                const auto& [named, reason] = *hookFailures.begin();
                failed.insert_or_assign(key, std::format("{}: {}", named, reason));
                namesOther.insert(key);
            }
        }
        takenBack.merge(assigned);
        assigned.clear();
    } else if (!verdict) {
        // merged blocks have each applied the keys they took, so a key one of them refused fails alone
        const auto failKey = [&](const std::string& key, const std::string& reason) {
            failed.insert_or_assign(key, reason);
            takenBack.insert(key);
        };
        for (const Rejection& entry : verdict.error()) {
            if (entry.key.empty()) {
                for (const auto& [key, _] : changedValues) {
                    failKey(std::string(key), entry.reason);
                }
            } else if (const std::string key = canonicalKey(_table, entry.key).value_or(entry.key); written.contains(key)) {
                failKey(key, entry.reason);
            }
        }
    }
    for (const std::string& key : takenBack) {
        const std::optional<KeyRef> ref = _table.find(key);
        if (!ref) {
            continue;
        }
        if (const auto old = oldFields.find(key); old != oldFields.end()) {
            std::ignore = writeField(*ref, key, old->second);
            // a hook that resized a port collection and then failed leaves the size it gave, so the collection takes the
            // size of the restored count
            for (const std::string& collection : ref->declaration->portCollections) {
                const auto          resize = _table.collectionResizes.find(collection);
                const std::uint64_t count  = countFrom(&old->second);
                if (resize != _table.collectionResizes.end() && std::in_range<std::size_t>(count)) {
                    resize->second(_block, static_cast<std::size_t>(count));
                }
            }
        } else {
            eraseField(*ref, key);
        }
        if (const auto old = oldPublished.find(key); old != oldPublished.end()) {
            _published.insert_or_assign(key, old->second);
        } else {
            _published.erase(key);
        }
    }
    // the initialization's apply sees every setting changed, so a hook that forwards on a change would name every value
    // it forwards, and a graph of such blocks would start with one tag from each that no caller asked for
    if (!batchFails && !atInit) {
        for (auto& [key, value] : forwards) {
            _toForward.insert_or_assign(key, std::move(value));
        }
    }
    if (taken->reset) {
        std::optional<std::string> resetThrown;
        try {
            runResetHook();
        } catch (const gr::exception& exception) {
            resetThrown = exception.message;
        } catch (const std::exception& exception) {
            resetThrown = exception.what();
        } catch (...) {
            resetThrown = "exception in reset()";
        }
        copyStatus();
        if (resetThrown) {
            notify(NotificationLevel::error, NotificationKind::failed, {}, *resetThrown);
        }
    }

    _applying.reset();
    for (const auto& [key, _] : taken->keys) {
        if (const std::optional<KeyRef> ref = _table.find(key); ref && ref->group == nullptr) {
            eraseMembersPastCount(*ref);
        }
    }
    publishedKeys.merge(writePublished());
    // in force moves by the keys the batch and the failures wrote; a field the hooks or a count changed besides fails the
    // check, and in force is read whole. The values a derived field is told against: the fields before, which an edited
    // in force still holds at every key the batch and the failures did not write; none when nothing else moved.
    std::optional<ValueMap> editedInForce;
    const ValueMap*         derivedAgainst = &oldFields;
    if (!readBefore) { // in force is edited in place, so oldFields is read no further
        for (const auto& [key, _] : taken->keys) {
            rereadField(_inForce, key);
        }
        for (const std::string& key : publishedKeys) {
            rereadField(_inForce, key);
        }
        derivedAgainst = nullptr;
        if (!fieldsHold(_inForce)) {
            editedInForce  = std::exchange(_inForce, readFields(false));
            derivedAgainst = &*editedInForce;
        }
    } else {
        _inForce = readFields(false);
    }
    const ValueMap& inForce = _inForce;

    // the setup's refusal names the key the block failed, as a refused constructed value does, and not every setting the
    // setup takes back with it
    const bool namesEveryKey = !atInit || namesOther.size() == failed.size();
    for (const auto& [key, reason] : failed) {
        _failures.insert_or_assign(key, settings::detail::Failure{.batch = batch, .reason = reason});
        if (namesEveryKey || !namesOther.contains(key)) {
            notify(NotificationLevel::error, NotificationKind::failed, key, reason);
        }
    }
    // a failed key keeps the record it held before the batch
    for (const auto& [key, change] : taken->keys) {
        if (!failed.contains(key) && change.given) {
            _record.insert_or_assign(key, change.value);
        }
    }
    for (const std::string& key : assigned) {
        if (const auto record = _record.find(key); record != _record.end()) {
            if (const auto now = inForce.find(key); now != inForce.end()) {
                record->second = now->second;
            }
        }
    }
    for (const auto& [key, _] : taken->keys) {
        if (failed.contains(key)) {
            continue;
        }
        const auto countValue = inForce.find(key);
        forEachCountedGroup(_table, key, [&](const MemberGroup& group) {
            const std::uint64_t count = countFrom(countValue != inForce.end() ? &countValue->second : nullptr);
            std::erase_if(_record, [&](const auto& entry) { return isMemberPast(_table, entry.first, group, count); });
        });
    }

    const auto level = inForce.find(kNotificationLevelKey);
    const auto named = level != inForce.end() ? parseNotificationLevel(level->second.value_or(std::string_view{})) : std::nullopt;
    _ownLevel.store(named ? static_cast<std::uint8_t>(*named) : kNoLevel, std::memory_order_relaxed);

    // the values a block is built with are no change a watcher follows
    if (!atInit && passes(NotificationLevel::info)) {
        for (const auto& [key, change] : taken->keys) {
            if (failed.contains(key)) {
                continue;
            }
            const std::optional<pmt::Value> now       = heldValue(key);
            const auto                      published = _published.find(key);
            const auto                      before    = previousValues.find(key);
            const bool                      byHook    = assigned.contains(key);
            // a key the batch did not change and whose read the hook did not move is no change a watcher follows. A key the
            // hook assigned is announced only where it differs from its value before the batch, as the caller's change
            if ((byHook || !changedValues.contains(key)) && before != previousValues.end() && now && sameValue(before->second, *now)) {
                continue;
            }
            if (!change.given) {
                notify(NotificationLevel::info, NotificationKind::changed, key, "a tag set the value");
            } else if (byHook || (now && sameValue(*now, change.value))) {
                notify(NotificationLevel::info, NotificationKind::changed, key, "a call set the value");
            } else {
                notify(NotificationLevel::info, NotificationKind::changed, key, published != _published.end() && !published->second.reason.empty() ? std::string_view(published->second.reason) : std::string_view("the value in force differs"));
            }
        }
        for (const auto& [key, value] : inForce) {
            const std::optional<KeyRef> ref = _table.find(key);
            if (derivedAgainst == nullptr || !ref || ref->declaration->published || taken->keys.contains(key) || publishedKeys.contains(key) || assigned.contains(key)) {
                continue;
            }
            if (const auto old = derivedAgainst->find(key); old == derivedAgainst->end() || !sameValue(old->second, value)) {
                notify(NotificationLevel::info, NotificationKind::changed, key, "the block derived the value");
            }
        }
    }

    _appliedBatch = std::max(_appliedBatch, batch);
    ++_version;
    // a block used alone whose init() failed keeps has work set; its next work call runs init() again
    storeHasWork(!_pending.empty() || !_toWrite.empty() || !_setUp); // a key the slot took during the hooks applies at the next work call
    wakeWaiters();

    Rejected failedKeys;
    for (auto& [key, reason] : failed) {
        if (namesEveryKey || !namesOther.contains(key)) {
            failedKeys.push_back(Rejection{.key = key, .reason = std::move(reason)});
        }
    }
    GraphLink* const relist = portsResized ? _graphLink : nullptr;
    lock.unlock();
    if (relist != nullptr) {
        relist->refreshPorts();
    }
    return failedKeys;
}

std::optional<std::string> SettingsBase::failedReason(std::string_view key, std::uint64_t batch, const pmt::Value& sent) const {
    const auto failure = _failures.find(key);
    if (failure == _failures.end()) {
        return std::nullopt;
    }
    if (failure->second.batch == batch) {
        return failure->second.reason;
    }
    // a later batch's failure replaced this one's; the key did not take when the value a read returns is not the one sent
    const std::optional<pmt::Value> now = heldValue(key);
    if (failure->second.batch > batch && (!now || !sameValue(*now, sent))) {
        return failure->second.reason;
    }
    return std::nullopt;
}

Values SettingsBase::resultOf(std::uint64_t batch, const ValueMap& sent) const {
    const std::string_view pendingCause = _state != BlockState::running ? kPendingNotRunning : kPendingTimeout;
    Values                 values;
    for (const auto& [key, value] : sent) {
        if (std::optional<std::string> reason = failedReason(key, batch, value); reason) {
            values.insert_or_assign(key, Value{.state = Value::State::failed, .value = {}, .reason = std::move(*reason)});
        } else if (_appliedBatch < batch) {
            values.insert_or_assign(key, Value{.state = Value::State::pending, .value = {}, .reason = std::string(pendingCause)});
        } else if (const std::optional<pmt::Value> now = heldValue(key); !now || !sameValue(*now, value)) {
            const auto published = _published.find(key);
            values.insert_or_assign(key, Value{.state = Value::State::value, .value = now.value_or(pmt::Value{}), .reason = published != _published.end() ? published->second.reason : std::string{}});
        }
    }
    return values;
}

void SettingsBase::wakeWaiters() {
    {
        const std::lock_guard settledLock(_settledLock);
        ++_settledCount;
    }
    _settled.notify_all();
}

bool SettingsBase::waitsForLock(Timeout timeout) const {
    const SchedulerLink* link = _schedulerLink.load(std::memory_order_acquire);
    return !(timeout && *timeout <= noWait) && !(link != nullptr && link->isWorkerThread());
}

std::expected<Values, Rejected> SettingsBase::slotChanges(const property_map& changes) {
    Rejected refused;
    for (const auto& [key, _] : changes) {
        if (const std::optional<KeyRef> ref = _table.find(key); !ref) {
            refused.push_back(invalidEntry(std::string(key), kUndeclared));
        } else if (ref->declaration->isReadOnly()) {
            refused.push_back(invalidEntry(tagKeyOf(*ref), ref->declaration->reported ? kStatus : kReadOnly));
        }
    }
    if (!refused.empty()) {
        return std::unexpected(std::move(refused));
    }
    Values pending;
    {
        const std::lock_guard slotLock(_slotLock);
        for (const auto& [key, value] : changes) {
            std::string tagKey = tagKeyOf(*_table.find(key));
            _slot.insert_or_assign(std::pmr::string(tagKey), value);
            pending.insert_or_assign(std::move(tagKey), Value{.state = Value::State::pending, .value = {}, .reason = std::string(kPendingQueued)});
        }
        _hasWork.store(true, std::memory_order_release);
    }
    if (SchedulerLink* link = _schedulerLink.load(std::memory_order_acquire); link != nullptr) {
        link->wake();
    }
    return pending;
}

void SettingsBase::drainSlot() {
    assert(holdsLock() && "the slot drains under the block lock");
    if (!_setUp || _applying || _draining) { // none before the setup, inside an apply, or inside a drain's check, whose hook may read the block
        return;
    }
    property_map slotted;
    {
        const std::lock_guard slotLock(_slotLock);
        slotted = std::exchange(_slot, property_map{});
    }
    if (slotted.empty()) {
        return;
    }
    _draining                                    = true;
    std::expected<CheckedCall, Rejected> checked = checkChanges(slotted);
    // no caller waits for the slot's keys: each refused key is notified and leaves the batch, and the other keys are
    // checked once more. A refusal that names no key, or one of the second check, drops the batch
    for (int round = 0; !checked && round < 2; ++round) {
        notifyRefused(checked.error());
        const bool namesEachKey = std::ranges::none_of(checked.error(), [](const Rejection& entry) { return entry.key.empty(); });
        for (const Rejection& entry : checked.error()) {
            if (const auto kept = slotted.find(entry.key); kept != slotted.end()) {
                slotted.erase(kept);
            }
        }
        if (round == 1 || !namesEachKey || slotted.empty()) {
            break;
        }
        checked = checkChanges(slotted);
    }
    _draining = false;
    if (checked) {
        std::ignore = merge(*checked, false, false);
    }
}

void SettingsBase::storeHasWork(bool hasWork) {
    const std::lock_guard slotLock(_slotLock);
    _hasWork.store(hasWork || !_slot.empty(), std::memory_order_release);
}

std::optional<Clock::time_point> SettingsBase::lockDeadline(Clock::time_point calledAt, Timeout timeout) const {
    const SchedulerLink*            link   = _schedulerLink.load(std::memory_order_acquire);
    const std::chrono::milliseconds wanted = timeout && *timeout > noWait ? *timeout : (link != nullptr ? link->defaultTimeout() : kDefaultTimeout);
    // `calledAt + wanted` would overflow the clock, so a timeout past the clock's range waits with no deadline
    if (wanted >= std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - calledAt)) {
        return std::nullopt;
    }
    return calledAt + wanted;
}

Rejected SettingsBase::lockTimedOut(const property_map& changes, Clock::time_point calledAt) const {
    const std::string reason = std::format("timed out after {} ms {}", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - calledAt).count(), kWaitingForLock);
    Rejected          refused;
    for (const auto& [key, _] : changes) {
        refused.push_back(invalidEntry(tagKeyOrSent(key), reason));
    }
    if (refused.empty()) { // a reset names no key
        refused.push_back(invalidEntry({}, reason));
    }
    return refused;
}

// the wait for the apply holds no block lock; it takes the lock only to judge the call, each time up to the deadline.
// A call returns at its timeout even while an apply hook still runs
Values SettingsBase::wait(const Merged& merged, std::optional<Clock::time_point> deadline) {
    while (true) {
        std::uint64_t seen = 0U;
        {
            const BlockLock lock(*this, deadline);
            if (!lock.held()) {
                return pendingFor(merged.sent, kPendingTimeout);
            }
            if (_appliedBatch >= merged.batch || _state != BlockState::running || (deadline && Clock::now() >= *deadline)) {
                return resultOf(merged.batch, merged.sent);
            }
            const std::lock_guard settledLock(_settledLock);
            seen = _settledCount;
        }
        std::unique_lock settledLock(_settledLock);
        const auto       moved = [&] { return _settledCount != seen; };
        if (deadline) {
            std::ignore = _settled.wait_until(settledLock, *deadline, moved);
        } else {
            _settled.wait(settledLock, moved);
        }
    }
}

Values SettingsBase::finish(const Merged& merged, Clock::time_point calledAt, Timeout timeout) {
    const bool                             waitsForApply = waitsForLock(timeout);
    const std::optional<Clock::time_point> deadline      = waitsForApply ? lockDeadline(calledAt, timeout) : std::optional(calledAt);
    {
        const BlockLock lock(*this, deadline);
        if (!lock.held()) { // the key is merged, and waits for the apply under way
            if (merged.waking && merged.schedulerLink != nullptr) {
                merged.schedulerLink->wake();
            }
            return pendingFor(merged.sent, waitsForApply ? kPendingTimeout : kPendingQueued);
        }
        if (applyAtCall(merged)) {
            return resultOf(merged.batch, merged.sent);
        }
    }
    if (_table.isScheduler && merged.stateNoted == BlockState::running) { // no apply comes before the next start
        return pendingFor(merged.sent, kPendingNextStart);
    }
    if (merged.waking && merged.schedulerLink != nullptr) {
        merged.schedulerLink->wake();
    }
    if (waitsForApply && merged.stateNoted == BlockState::running && !merged.sent.empty()) {
        return wait(merged, deadline);
    }
    const std::string_view cause = merged.stateNoted != BlockState::running ? kPendingNotRunning : (waitsForApply ? kPendingTimeout : kPendingOnWorker);
    return pendingFor(merged.sent, cause);
}

std::expected<Values, Rejected> SettingsBase::set(const property_map& changes, Timeout timeout) {
    if (onOwnThread() || holdsLock()) { // a hook runs under this thread's hold
        return std::unexpected(Rejected{invalidEntry({}, kOwnCall)});
    }
    const Clock::time_point calledAt = Clock::now();
    // a call that does not wait only tries the block lock, and leaves its keys in the slot while another thread holds it;
    // while another thread holds the block lock, a composite's parameters go on to the inner blocks without this block's setup
    const bool                             namesParameter = _composite != nullptr && _composite->namesParameter(changes);
    const bool                             slots          = !waitsForLock(timeout);
    const std::optional<Clock::time_point> deadline       = slots ? std::optional(calledAt) : lockDeadline(calledAt, timeout);
    {
        const BlockLock lock(*this, deadline);
        if (!lock.held()) {
            if (slots) {
                return namesParameter ? _composite->set(*this, changes, calledAt, timeout) : slotChanges(changes);
            }
            return std::unexpected(lockTimedOut(changes, calledAt));
        }
        if (std::expected<void, Rejected> used = firstUse(); !used) {
            if (std::optional<std::expected<Values, Rejected>> judged = setUpByCall(changes, true); judged) {
                return std::move(*judged);
            }
        }
    }
    if (namesParameter) { // the composite lock comes before the block lock
        return _composite->set(*this, changes, calledAt, timeout);
    }
    std::expected<Merged, Rejected> merged;
    {
        const BlockLock lock(*this, deadline);
        if (!lock.held()) {
            if (slots) {
                return slotChanges(changes);
            }
            return std::unexpected(lockTimedOut(changes, calledAt));
        }
        merged = checkAndMerge(changes, nullptr, false);
    }
    if (!merged) {
        return std::unexpected(std::move(merged.error()));
    }
    return finish(*merged, calledAt, timeout);
}

void SettingsBase::notifyRefused(const Rejected& refused) const {
    for (const Rejection& entry : refused) {
        notify(NotificationLevel::error, NotificationKind::failed, entry.key, entry.reason);
    }
}

std::expected<Values, Rejected> SettingsBase::set(std::string_view key, pmt::Value value, Timeout timeout) { return set(property_map{{std::pmr::string(key), std::move(value)}}, timeout); }

std::optional<std::expected<Values, Rejected>> SettingsBase::setUpByCall(const property_map& changes, bool store) {
    const BlockLock lock(*this); // the setup and its answer in one hold
    if (_setUp) {
        return std::nullopt;
    }
    const settings::detail::OwnThread applying(*this);
    SetUpVerdict                      verdict = setUpWith(changes, store);
    if (!verdict.entries.empty()) {
        return std::unexpected(std::move(verdict.entries));
    }
    ValueMap sent;
    for (const auto& [key, _] : changes) {
        if (const auto it = verdict.merged.sent.find(tagKeyOrSent(key)); it != verdict.merged.sent.end()) {
            sent.insert(*it);
        }
    }
    return resultOf(verdict.merged.batch, sent);
}

std::expected<void, Rejected> SettingsBase::check(const property_map& changes) {
    if (holdsLock()) { // a hook runs under this thread's hold, and a check would run its block's check hook inside it
        return std::unexpected(Rejected{invalidEntry({}, kOwnCall)});
    }
    return checkWithin(changes, std::nullopt, Clock::now());
}

std::expected<void, Rejected> SettingsBase::checkWithin(const property_map& changes, std::optional<Clock::time_point> deadline, Clock::time_point calledAt) {
    {
        const BlockLock lock(*this, deadline);
        if (!lock.held()) {
            return std::unexpected(lockTimedOut(changes, calledAt));
        }
        if (std::expected<void, Rejected> used = firstUse(); !used) {
            if (std::optional<std::expected<Values, Rejected>> judged = setUpByCall(changes, false); judged) {
                const auto givenByCall     = [&](const Rejection& entry) { return std::ranges::any_of(changes, [&](const auto& change) { return tagKeyOrSent(change.first) == entry.key; }); };
                const bool replacesRefused = std::ranges::any_of(used.error(), givenByCall);
                if (judged->has_value() && !heldNotReady() && !replacesRefused) { // the setup's apply hook refused what the block holds
                    return std::unexpected(std::move(used.error()));
                }
                return judged->transform([](const Values&) {});
            }
        }
    }
    if (_composite != nullptr && _composite->namesParameter(changes)) { // the composite lock comes before the block lock
        return _composite->check(*this, changes, deadline, calledAt);
    }
    const BlockLock lock(*this, deadline);
    if (!lock.held()) {
        return std::unexpected(lockTimedOut(changes, calledAt));
    }
    return checkChanges(changes).transform([](const CheckedCall&) {});
}

std::expected<Value, Rejected> SettingsBase::get(std::string_view key) const {
    const std::array<std::string, 1UZ> keys{std::string(key)};
    return get(std::span<const std::string>(keys)).transform([](Values values) { return std::move(values.begin()->second); });
}

std::expected<Values, Rejected> SettingsBase::get(std::span<const std::string> keys) const {
    if (_composite == nullptr) {
        return readOwn(keys);
    }
    std::vector<std::string> ownKeys;
    Values                   parameters = _composite->readParameters(keys, ownKeys); // the composite lock is released before the block lock is taken
    if (!keys.empty() && ownKeys.empty()) {
        return parameters;
    }
    std::expected<Values, Rejected> own = readOwn(ownKeys);
    if (own) {
        own->merge(parameters);
    }
    return own;
}

std::expected<Values, Rejected> SettingsBase::readOwn(std::span<const std::string> keys) const {
    struct SourceEntry {
        pmt::Value value{};
        bool       waiting = false; // waits for the block's start or resume
    };
    // the whole read runs under the block lock, so an apply or a setup under way on another thread ends before it
    const BlockLock lock(*this);
    const_cast<SettingsBase&>(*this).drainSlot(); // a read finds the slot's keys merged, as a set's would be
    std::map<std::string, SourceEntry, std::less<>> source;
    if (!_setUp) {
        for (auto& [key, value] : sourceBeforeInit()) {
            source.insert_or_assign(key, SourceEntry{.value = std::move(value)});
        }
    } else {
        for (const auto& [key, value] : _inForce) {
            source.insert_or_assign(key, SourceEntry{.value = value});
        }
        if (!_fieldsEnded) { // a published key reads its field, and a value another thread published waits to be written there
            for (auto& [key, value] : readFields(true)) {
                source.insert_or_assign(key, SourceEntry{.value = std::move(value)});
            }
            for (const auto& [key, value] : _toWrite) {
                if (isPublishedKey(key) && value.has_value()) {
                    source.insert_or_assign(key, SourceEntry{.value = value});
                } else if (isPublishedKey(key)) {
                    source.erase(key);
                }
            }
        }
        for (const auto& [key, published] : _published) {
            source.insert_or_assign(key, SourceEntry{.value = published.value});
        }
        if (_state != BlockState::running && (!_pending.keys.empty() || _applying)) {
            const auto overlay = [&source](const PendingChanges& changes) {
                for (const auto& [key, change] : changes.keys) {
                    source.insert_or_assign(key, SourceEntry{.value = change.value, .waiting = true});
                }
            };
            if (_applying) {
                overlay(*_applying);
            }
            overlay(_pending);
            for (const auto& [countKey, change] : _pending.keys) {
                forEachCountedGroup(_table, countKey, [&](const MemberGroup& group) {
                    const std::uint64_t count = countFrom(&change.value);
                    std::erase_if(source, [&](const auto& entry) { return isMemberPast(_table, entry.first, group, count); });
                });
            }
        }
    }
    const auto entryOf = [&](const std::string& tagKey, const SourceEntry& found) {
        const auto published = _published.find(tagKey);
        return Value{.state = Value::State::value, .value = found.value, .reason = !found.waiting && published != _published.end() ? published->second.reason : std::string{}};
    };

    // a host that reads a status value has the next work call or message pass copy the block's `Status`
    const auto wantStatus = [this] { _statusWanted.store(true, std::memory_order_relaxed); };
    Values     values;
    if (keys.empty()) {
        if (std::ranges::any_of(_table.keys, [](const auto& entry) { return entry.second.reported; })) {
            wantStatus();
        }
        for (const auto& [key, entry] : source) {
            values.insert_or_assign(key, entryOf(key, entry));
        }
        return values;
    }
    Rejected rejected;
    for (const std::string& asked : keys) {
        const std::optional<KeyRef> ref = _table.find(asked);
        if (!ref) {
            rejected.push_back(invalidEntry(asked, kUndeclared));
            continue;
        }
        if (ref->group != nullptr && !ref->group->countKey.empty()) {
            const auto count = source.find(ref->group->countKey);
            if (ref->member >= countFrom(count != source.end() ? &count->second.value : nullptr)) {
                rejected.push_back(invalidEntry(asked, kUndeclared));
                continue;
            }
        }
        if (ref->declaration->reported) {
            wantStatus();
        }
        const std::string tagKey = tagKeyOf(*ref);
        const auto        found  = source.find(tagKey);
        if (found == source.end()) {
            const bool applying = _applying && _applying->keys.contains(tagKey);
            if (applying || (_pending.keys.contains(tagKey) && _state == BlockState::running)) { // a read waits for nothing, so it gives no cause
                values.insert_or_assign(asked, Value{.state = Value::State::pending, .value = {}, .reason = {}});
            } else {
                rejected.push_back(invalidEntry(asked, kNoValue));
            }
            continue;
        }
        values.insert_or_assign(asked, entryOf(tagKey, found->second));
    }
    if (!rejected.empty()) {
        return std::unexpected(std::move(rejected));
    }
    return values;
}

std::expected<Values, Rejected> SettingsBase::resetDefaults(Timeout timeout) {
    if (onOwnThread() || holdsLock()) {
        return std::unexpected(Rejected{invalidEntry({}, kOwnCall)});
    }
    const Clock::time_point         calledAt = Clock::now();
    std::expected<Merged, Rejected> merged;
    {
        const BlockLock lock(*this, lockDeadline(calledAt, timeout));
        if (!lock.held()) {
            return std::unexpected(lockTimedOut({}, calledAt));
        }
        if (std::expected<void, Rejected> used = firstUse(); !used) {
            return std::unexpected(std::move(used.error()));
        }
        property_map defaults;
        for (const auto& [key, value] : _storedDefaults) {
            defaults.insert_or_assign(std::pmr::string(key), value);
        }
        merged = checkAndMerge(defaults, nullptr, true);
    }
    if (!merged) {
        return std::unexpected(std::move(merged.error()));
    }
    return finish(*merged, calledAt, timeout);
}

Rejected SettingsBase::applyWhenDue() {
    GraphLink* graphLink = nullptr;
    {
        const BlockLock lock(*this);
        if (!_notReady.empty()) { // a block that is not ready applies nothing until a `set` makes it ready
            return _notReady;
        }
        graphLink = _graphLink;
    }
    if (graphLink == nullptr) {
        if (std::expected<void, Rejected> used = firstUse(); !used) {
            return std::move(used.error());
        }
    }
    return apply();
}

Rejected SettingsBase::tagEnters(const property_map& coupled, bool resetDefaults) {
    property_map    changes;
    KeySet          tagSets;
    const BlockLock lock(*this); // the filter below, the check and the merge in one hold
    drainSlot();                 // the filter reads the slot's keys as pending
    {
        for (const auto& [key, sent] : coupled) {
            const std::optional<KeyRef> ref = _table.find(key);
            if (!ref) {
                changes.insert_or_assign(key, sent);
                continue;
            }
            const std::string tagKey = tagKeyOf(*ref);
            tagSets.insert(tagKey);
            std::expected<pmt::Value, std::string> converted = convert(*ref->declaration, sent);
            if (!converted) { // the check refuses the value as sent
                changes.insert_or_assign(std::pmr::string(tagKey), sent);
                continue;
            }
            // a relay republishes every tag it receives, so a tag at the value in force changes nothing unless it replaces a pending value
            if (!_pending.keys.contains(tagKey) && holdsInForce(tagKey, *converted)) {
                continue;
            }
            changes.insert_or_assign(std::pmr::string(tagKey), std::move(*converted));
        }
        if (resetDefaults) {
            for (const auto& [key, value] : _storedDefaults) {
                changes.try_emplace(std::pmr::string(key), value);
            }
        }
    }
    if (!changes.empty() || resetDefaults) {
        if (std::expected<Merged, Rejected> merged = checkAndMerge(changes, &tagSets, resetDefaults); !merged) {
            for (const Rejection& entry : merged.error()) {
                notify(NotificationLevel::error, NotificationKind::failed, entry.key, std::format("tag value refused: {}", entry.reason));
            }
            return std::move(merged.error());
        }
    }
    return {};
}

void SettingsBase::forwardTag(std::string_view tagKey, pmt::Value value) {
    assert(_forwarding != nullptr && "a block forwards a tag from its settings hook");
    if (_forwarding != nullptr) {
        _forwarding->insert_or_assign(std::pmr::string(tagKey), std::move(value));
    }
}

void SettingsBase::publish(std::string_view key, pmt::Value value, std::string_view reason) {
    const std::optional<KeyRef> ref       = _table.find(key);
    const std::string           memberKey = ref && ref->group != nullptr ? tagKeyOf(*ref) : std::string{}; // a block-wide key is its declaration's name
    const std::string_view      tagKey    = !ref ? key : (ref->group == nullptr ? std::string_view(ref->declaration->name) : std::string_view(memberKey));
    if (holdsLock()) { // a settings hook runs under the hold of this thread
        notify(NotificationLevel::error, NotificationKind::failed, tagKey, kPublishInApply);
        return;
    }
    if (ref && !holdsDeclaredType(*ref->declaration, value)) {
        std::expected<pmt::Value, std::string> typed = convert(*ref->declaration, value);
        if (!typed) { // the field cannot hold the value, so in force keeps what it holds
            const BlockLock lock(*this);
            notify(NotificationLevel::error, NotificationKind::failed, tagKey, typed.error());
            return;
        }
        value = std::move(*typed);
    }
    const BlockLock lock(*this);
    if (!ref) {
        notify(NotificationLevel::error, NotificationKind::failed, tagKey, kUndeclared);
        return;
    }
    // the block's own thread publishes a derived value without a guard around the call; a report from another thread is
    // announced below, since a host reads it as the device's confirmation
    if (onOwnThread()) {
        if (const std::optional<pmt::Value> held = heldValue(tagKey); held && sameValue(*held, value)) {
            return;
        }
    }
    if (!ref->declaration->published) { // the value last set stays in force, and a read returns this one beside it
        assignAt(_published, tagKey, settings::detail::PublishedValue{.value = std::move(value), .reason = std::string(reason)});
    } else if (onOwnThread()) { // a read-only key the block holds in its field takes the value at the call
        if (const auto pending = _toWrite.find(tagKey); pending != _toWrite.end()) {
            _toWrite.erase(pending);
        }
        std::ignore = writeField(*ref, tagKey, value);
    } else {
        assignAt(_toWrite, tagKey, std::move(value));
        _hasWork.store(true, std::memory_order_release);
    }
    notify(NotificationLevel::info, NotificationKind::changed, tagKey, reason.empty() ? std::string_view("the block published the value") : reason);
    if (!ref->declaration->isReadOnly()) {
        ++_version;
    }
    wakeWaiters();
    if (SchedulerLink* link = _schedulerLink.load(std::memory_order_acquire); link != nullptr) {
        link->wake();
    }
}

// the flag falls before the block's members are read: a host's read during the copy raises it again for the next work
// call or message pass. In the block's base destructor the block's own members, its `Status` among them, are already
// destroyed
void SettingsBase::copyStatus() {
    if (_fieldsEnded) {
        return;
    }
    _statusWanted.store(passes(NotificationLevel::info), std::memory_order_relaxed);
    publishStatus();
}

void SettingsBase::reportStatus(std::span<const StatusValue> values) {
    const BlockLock lock(*this);
    bool            moved = false;
    for (const StatusValue& entry : values) {
        const auto published = _published.find(entry.key);
        if (published != _published.end() && sameValue(published->second.value, entry.value)) {
            continue;
        }
        const bool replaces = published != _published.end(); // the setup's first copy is no change a watcher follows
        assignAt(_published, entry.key, settings::detail::PublishedValue{.value = entry.value});
        if (replaces) {
            notify(NotificationLevel::info, NotificationKind::changed, entry.key, "the block reported the value");
        }
        moved = true;
    }
    if (moved) {
        wakeWaiters();
    }
}

void SettingsBase::fail(std::string_view key, std::string_view reason) {
    const std::optional<KeyRef> ref    = _table.find(key);
    const std::string           tagKey = ref ? tagKeyOf(*ref) : std::string(key);
    const BlockLock             lock(*this);
    if (_hookFailures != nullptr) { // the apply decides what a failure in its hook takes back; only its thread holds the lock meanwhile
        _hookFailures->insert_or_assign(tagKey, std::string(reason));
    } else if (_starting) {
        failAtStart(tagKey, reason);
    } else {
        failLater(tagKey, reason);
    }
}

void SettingsBase::failLater(std::string_view tagKey, std::string_view reason) {
    _failures.insert_or_assign(std::string(tagKey), settings::detail::Failure{.batch = _applyingBatch, .reason = std::string(reason)});
    if (const auto it = _published.find(tagKey); it != _published.end()) {
        _published.erase(it);
    }
    notify(NotificationLevel::error, NotificationKind::failed, tagKey, reason);
    ++_version;
    wakeWaiters();
    if (SchedulerLink* link = _schedulerLink.load(std::memory_order_acquire); link != nullptr) {
        link->wake();
    }
}

// a key the device lacks leaves in force and the block's map
void SettingsBase::failAtStart(std::string_view tagKey, std::string_view reason) {
    _failures.insert_or_assign(std::string(tagKey), settings::detail::Failure{.batch = _run.batch, .reason = std::string(reason)});
    if (const auto record = _record.find(tagKey); record != _record.end()) {
        _record.erase(record);
    }
    assignAt(_run.keys, tagKey, pmt::Value{});
    if (const auto inForce = _inForce.find(tagKey); inForce != _inForce.end()) {
        _inForce.erase(inForce);
    }
    assignAt(_toWrite, tagKey, pmt::Value{});
    _hasWork.store(true, std::memory_order_release);
    notify(NotificationLevel::error, NotificationKind::failed, tagKey, reason);
}

Rejected SettingsBase::beginRun(bool isStart) {
    bool     pendingAny    = false;
    bool     checkedBefore = false; // a scheduler's check before its start found the state this start runs with
    ValueMap pendingValues;
    {
        const BlockLock lock(*this);
        if (!_notReady.empty()) {
            _run = settings::detail::Run{.isStart = isStart, .refusals = _notReady};
            return _run.refusals;
        }
        drainSlot(); // a key the slot holds applies with the start; its merge moves the version, so the check made before the start runs again
        checkedBefore = isStart && _startChecked == _version;
        _startChecked.reset();
        std::ignore = writePublished();
        _run        = settings::detail::Run{.isStart = isStart};
        if (isStart) { // the start hook opens the device again, so the previous run's marks end here
            std::erase_if(_failures, [this](const auto& entry) { return !_inForce.contains(entry.first); });
            _starting = true;
            ++_version;
        }
        for (const auto& [key, change] : _pending.keys) {
            pendingValues.insert_or_assign(key, change.value);
        }
        if (!isStart) {
            _run.keys = pendingValues;
        }
        pendingAny = !_pending.empty();
        if (pendingAny) {
            _run.batch = _nextBatch;
        } else if (isStart) { // a start has a batch of its own, which a failure of the start hook carries
            _run.batch     = _nextBatch++;
            _applyingBatch = _run.batch;
            _appliedBatch  = std::max(_appliedBatch, _run.batch);
        }
    }
    const Rejected failed = pendingAny ? apply() : Rejected{};
    if (!isStart) {
        return {};
    }
    {
        const BlockLock lock(*this);
        for (const auto& [key, _] : _record) {
            if (const auto inForce = _inForce.find(key); inForce != _inForce.end()) {
                _run.keys.insert_or_assign(key, inForce->second);
            }
        }
        for (const Rejection& entry : failed) {
            if (const auto sent = pendingValues.find(entry.key); sent != pendingValues.end()) {
                _run.keys.insert_or_assign(entry.key, sent->second);
            }
        }
    }
    if (checkedBefore) {
        return {};
    }
    Rejected        refusals = wholeStateRefusals();
    const BlockLock lock(*this);
    _run.refusals = std::move(refusals);
    return _run.refusals;
}

Rejected SettingsBase::checkStart() {
    Rejected refusals;
    {
        const BlockLock lock(*this);
        refusals = _notReady;
    }
    if (refusals.empty()) {
        refusals = wholeStateRefusals();
    }
    const BlockLock lock(*this);
    _run          = settings::detail::Run{.isStart = true, .refusals = refusals};
    _startChecked = refusals.empty() ? std::optional(_version) : std::nullopt;
    return refusals;
}

Rejected SettingsBase::wholeStateRefusals() {
    if (!hasCheckHook()) {
        return {};
    }
    const BlockLock               lock(*this);
    const CheckState              held{.pending = _pending, .applying = _applying.get(), .inForce = _inForce, .published = _published, .graphLink = _graphLink, .state = _state, .starting = _starting};
    const ValueMap                noChanges;
    const KeySet                  noneDisplaced;
    std::expected<void, Rejected> verdict = refusalOrThrown([&] {
        const CallView view(_table, noChanges, noneDisplaced, held, true);
        return runCheck(view);
    });
    if (verdict) {
        return {};
    }
    for (Rejection& entry : verdict.error()) {
        entry.key = entry.key.empty() ? std::string{} : tagKeyOrSent(entry.key);
    }
    return std::move(verdict.error());
}

void SettingsBase::endRun(bool isStart, bool refused, std::optional<std::string> hookFailure) {
    const BlockLock lock(*this);
    if (hookFailure) {
        _run.hookFailure = std::move(hookFailure);
        _state           = BlockState::error;
        ++_version;
    } else if (!refused) {
        _state = BlockState::running;
        ++_version;
    }
    std::ignore = writePublished();
    if (isStart) {
        _starting = false;
        ++_version;
    }
    wakeWaiters();
}

Values SettingsBase::runOutcome() const {
    const BlockLock lock(*this);
    Values          values;
    for (const auto& [key, sent] : _run.keys) {
        if (std::optional<std::string> reason = _run.batch != 0U ? failedReason(key, _run.batch, sent) : std::nullopt; reason) {
            values.insert_or_assign(key, Value{.state = Value::State::failed, .value = {}, .reason = std::move(*reason)});
        } else if (const std::optional<pmt::Value> now = heldValue(key); !now || !sameValue(*now, sent)) {
            const auto published = _published.find(key);
            values.insert_or_assign(key, Value{.state = Value::State::value, .value = now.value_or(pmt::Value{}), .reason = published != _published.end() ? published->second.reason : std::string{}});
        }
    }
    return values;
}

Rejected SettingsBase::runRefusals() const {
    const BlockLock lock(*this);
    Rejected        refusals = _run.refusals;
    if (_run.hookFailure) {
        refusals.push_back(Rejection{.key = {}, .reason = *_run.hookFailure});
    }
    return refusals;
}

bool SettingsBase::workerApplies() const {
    const BlockLock lock(*this);
    return _graphLink == nullptr || _state == BlockState::running;
}

bool SettingsBase::isRunning() const {
    const BlockLock lock(*this);
    return _state == BlockState::running;
}

bool SettingsBase::isGiven(std::string_view key) const {
    if (_composite != nullptr && _composite->declares(key)) {
        return _composite->isGiven(key);
    }
    const std::string tagKey  = tagKeyOrSent(key);
    const auto        givenIn = [&tagKey](const PendingChanges& changes) {
        const auto it = changes.keys.find(tagKey);
        return it != changes.keys.end() && it->second.given;
    };
    const BlockLock lock(*this);
    return _record.contains(tagKey) || givenIn(_pending) || (_applying && givenIn(*_applying));
}

void SettingsBase::moveState(BlockState state) {
    const BlockLock lock(*this);
    if (state == BlockState::stopped && (_state == BlockState::running || _state == BlockState::paused) && !_fieldsEnded) {
        restoreUnpublishedFields(readFields(false));
    }
    _state = state;
    if (state == BlockState::stopped || state == BlockState::error) { // a start that ends there ends its starting mark
        _starting = false;
    }
    ++_version;
    wakeWaiters();
}

void SettingsBase::endFields() {
    const BlockLock lock(*this);
    _fieldsEnded = true;
}

void SettingsBase::markStarting(bool starting) {
    const BlockLock lock(*this);
    _starting = starting;
    ++_version;
}

void SettingsBase::attachGraph(GraphLink* link) {
    const BlockLock lock(*this);
    _graphLink = link;
}

void SettingsBase::detachGraph(const GraphLink* link) {
    const BlockLock lock(*this);
    if (_graphLink == link) {
        _graphLink = nullptr;
    }
}

void SettingsBase::attachScheduler(SchedulerLink* link) { _schedulerLink.store(link, std::memory_order_release); }

void SettingsBase::detachScheduler(SchedulerLink* link) {
    SchedulerLink* attached = link;
    _schedulerLink.compare_exchange_strong(attached, nullptr, std::memory_order_acq_rel);
}

property_map SettingsBase::takeForwarded() { return std::exchange(_toForward, property_map{}); }

property_map SettingsBase::savedValues() const {
    property_map    values = _composite != nullptr ? _composite->record() : property_map{}; // the composite lock is released before the block lock is taken
    const BlockLock lock(*this);
    const_cast<SettingsBase&>(*this).drainSlot(); // a save holds the slot's keys, as a set's would be
    if (!_setUp) {
        for (const auto& [key, sent] : _constructedWith) {
            if (const std::optional<KeyRef> ref = _table.find(key); ref && !ref->declaration->isReadOnly()) {
                values.insert_or_assign(std::pmr::string(tagKeyOf(*ref)), sent);
            }
        }
        return values;
    }
    for (const auto& [key, value] : _record) {
        values.insert_or_assign(std::pmr::string(key), value);
    }
    const auto overlay = [&values](const PendingChanges& changes) {
        for (const auto& [key, change] : changes.keys) {
            if (change.given) {
                values.insert_or_assign(std::pmr::string(key), change.value);
            }
        }
    };
    if (_applying) {
        overlay(*_applying);
    }
    overlay(_pending);
    // a count waiting to apply removes the members past it, as its apply removes them from the record
    for (const auto& changes : {_applying.get(), &_pending}) {
        if (changes == nullptr) {
            continue;
        }
        for (const auto& [countKey, change] : changes->keys) {
            forEachCountedGroup(_table, countKey, [&](const MemberGroup& group) {
                const std::uint64_t count = countFrom(&change.value);
                std::erase_if(values, [&](const auto& entry) { return isMemberPast(_table, entry.first, group, count); });
            });
        }
    }
    return values;
}

std::vector<std::string> settings::detail::FrameworkAccess::parameterNames(const SettingsBase& settings) { return settings._composite != nullptr ? settings._composite->parameterNames() : std::vector<std::string>{}; }

std::set<std::string> SettingsBase::writableMembers() const {
    std::set<std::string> names;
    if (_composite != nullptr) {
        std::ranges::copy(_composite->parameterNames(), std::inserter(names, names.end()));
    }
    for (const auto& [name, declaration] : _table.keys) {
        if (!declaration.isReadOnly()) {
            names.insert(name);
        }
    }
    if (_table.groups.empty()) {
        return names;
    }
    const BlockLock lock(*this);
    const ValueMap  source = _setUp ? _inForce : sourceBeforeInit();
    for (const auto& [key, _] : source) {
        if (const std::optional<KeyRef> ref = _table.find(key); ref && ref->group != nullptr && !ref->declaration->isReadOnly()) {
            names.insert(key);
        }
    }
    return names;
}

void SettingsBase::describe() {
    const BlockLock lock(*this);
    describeBlock();
}

namespace {
// why `key` cannot be a coupling's tag key, or nothing for a key that can
[[nodiscard]] std::optional<std::string_view> refuseTagKey(std::string_view key) {
    const bool printable = std::ranges::all_of(key, [](char c) { return static_cast<unsigned char>(c) > 0x20U && c != 0x7F; });
    if (key.starts_with(':') || gr::tag::nameOf(key).empty() || !printable) {
        return "not domain:name without spaces";
    }
    if (std::ranges::contains(gr::tag::kControlKeys, key)) {
        return "reserved by the framework";
    }
    return std::nullopt;
}
} // namespace

std::expected<void, Rejected> SettingsBase::setTagCouplings(const property_map& couplings) {
    if (holdsLock()) { // a hook runs under this thread's hold, and a composite's coupling takes the composite lock after it
        return std::unexpected(Rejected{invalidEntry({}, kOwnCall)});
    }
    Rejected                                        refused;
    std::map<std::string, std::string, std::less<>> changes;    // an empty tag key removes the coupling
    std::map<std::string, std::string, std::less<>> parameters; // a recipe composite's, which it keeps
    for (const auto& [setting, key] : couplings) {
        const std::optional<KeyRef> ref         = _table.find(setting);
        const bool                  isParameter = !ref && _composite != nullptr && _composite->declares(setting); // a tag that enters the composite sets it
        if (!isParameter && (!ref || ref->declaration->isReadOnly())) {
            refused.push_back(invalidEntry(std::string(setting), ref && ref->declaration->reported ? kStatus : "not writable"));
            continue;
        }
        const std::string      settingKey = isParameter ? std::string(std::string_view(setting)) : tagKeyOf(*ref);
        const std::string_view spelled    = key.value_or(std::string_view{});
        if (spelled.data() == nullptr) {
            refused.push_back(invalidEntry(settingKey, std::format("tag key not a text: {}", key)));
            continue;
        }
        const std::string_view canonical = gr::tag::canonicalKey(spelled);
        if (!spelled.empty()) { // an empty key removes the coupling; a lone colon is a malformed key
            if (const std::optional<std::string_view> reason = refuseTagKey(canonical)) {
                refused.push_back(invalidEntry(settingKey, std::format("tag key {}: {}", spelled, *reason)));
                continue;
            }
        }
        (isParameter ? parameters : changes).insert_or_assign(settingKey, std::string(canonical));
    }
    if (!refused.empty()) {
        return std::unexpected(std::move(refused));
    }
    for (auto& [parameter, key] : parameters) {
        _composite->couple(parameter, std::move(key));
    }
    const BlockLock lock(*this);
    for (auto& [setting, key] : changes) {
        if (key.empty()) {
            _nTagReasons.fetch_sub(_couplings.erase(setting));
        } else if (_couplings.insert_or_assign(setting, std::move(key)).second) {
            _nTagReasons.fetch_add(1UZ);
        }
    }
    return {};
}

property_map SettingsBase::tagCouplings() const {
    property_map    table = _composite != nullptr ? _composite->couplings() : property_map{}; // the composite lock is released before the block lock is taken
    const BlockLock lock(*this);
    for (const auto& [setting, key] : _couplings) {
        table.insert_or_assign(std::pmr::string(setting), pmt::Value(key));
    }
    return table;
}

Rejected SettingsBase::tagEnters(const property_map& tagMap) {
    // the tag's own comparison takes each spelling of its key
    const bool resetDefaults = std::ranges::any_of(tagMap, [](const auto& entry) { return entry.first == gr::tag::RESET_DEFAULTS; });
    // a block that follows no tag key takes no lock here; the next apply drains the slot
    if (_nTagReasons.load(std::memory_order_acquire) == 0UZ && !resetDefaults) {
        return {};
    }
    // the composite translates a tag where its input enters, before this block reads its own couplings
    settings::detail::CompositeSettings* const entered    = _compositeEntry.load();
    const property_map                         translated = entered != nullptr ? entered->translateTag(*this, tagMap) : property_map{};
    property_map                               coupled;
    {
        const BlockLock lock(*this);
        drainSlot(); // a tag that restates the value in force replaces a value the slot held
        // a tag at the value in force changes nothing unless it replaces a pending value, so it allocates nothing; a
        // member key's tag key is built, and its value takes the check below, as does every key of a tag that resets
        // the defaults, where the key the tag carries decides whether its default is given
        const auto restatesInForce = [this, resetDefaults](std::string_view setting, const pmt::Value& sent) {
            const std::optional<KeyRef> ref = _table.find(setting);
            if (resetDefaults || !ref || ref->group != nullptr) {
                return false;
            }
            const std::string_view tagKey = ref->declaration->name;
            if (_pending.keys.contains(tagKey)) {
                return false;
            }
            const std::expected<pmt::Value, std::string> converted = convert(*ref->declaration, sent);
            return converted.has_value() && holdsInForce(tagKey, *converted);
        };
        for (const auto& [setting, key] : _couplings) {
            if (const auto tagged = tagMap.find(std::string_view(key)); tagged != tagMap.end() && !restatesInForce(setting, tagged->second)) {
                coupled.insert_or_assign(std::pmr::string(setting), tagged->second);
            }
        }
        for (const auto& [innerKey, setting] : _innerTagKeys) {
            if (const auto tagged = tagMap.find(std::string_view(innerKey)); tagged != tagMap.end()) {
                coupled.insert_or_assign(std::pmr::string(setting), tagged->second);
            } else if (const auto own = translated.find(std::string_view(innerKey)); own != translated.end()) {
                coupled.insert_or_assign(std::pmr::string(setting), own->second);
            }
        }
        for (const auto& [innerKey, value] : translated) { // the inner tag keys follow the stream to the inner blocks after this one
            _toForward.insert_or_assign(innerKey, value);
        }
        if (!translated.empty()) { // the next apply forwards them at the tag's sample, though this block sets nothing
            _hasWork.store(true, std::memory_order_release);
        }
    }
    if (coupled.empty() && !resetDefaults) {
        return {};
    }
    return tagEnters(coupled, resetDefaults);
}

} // namespace gr
