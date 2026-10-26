#include <gnuradio-4.0/LifeCycle.hpp>
#include <gnuradio-4.0/Settings.hpp>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <map>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gr {

namespace settings {
void throwInvalidContextType(const pmt::Value& value) { throw gr::exception("Invalid CtxSettings context type " + std::string(typeid(value).name())); }

bool recordAppliedValue(std::string_view key, const pmt::Value& stagedValue, property_map& appliedParameters, property_map& stagedForCallback, bool hasSettingsChangedCallback) {
    const auto keyPmr = std::pmr::string(key);
    appliedParameters.insert_or_assign(keyPmr, stagedValue);
    if (hasSettingsChangedCallback) {
        stagedForCallback.insert_or_assign(keyPmr, stagedValue);
    }
    return true;
}

BlockDescriptor::BlockDescriptor(const BlockHooks& blockHooks) : hooks(blockHooks) {
    for (const MemberDescriptor& member : hooks.members) {
        if (member.setParameter != nullptr) {
            writableMembers.emplace(member.name);
            writableByName.emplace(member.name, &member);
        }
        if (member.readParameter != nullptr) {
            readableMembers.push_back(&member);
        }
    }
}

} // namespace settings

namespace {
// The reason a set for `ctx` would pass kMaxContexts or kMaxPendingSets, or nothing when it fits.
// `forLaterTime` says the caller asked for a time after `now`; only such a set counts against kMaxPendingSets.
[[nodiscard]] std::optional<std::string> refuseStoredSet(const std::map<pmt::Value, std::vector<SettingsBase::CtxSettingsPair>, settings::PMTCompare>& stored, const SettingsCtx& ctx, bool forLaterTime, std::uint64_t now) {
    const auto contextIt = stored.find(ctx.context);
    if (contextIt == stored.end() || contextIt->second.empty()) {
        const auto nContexts = static_cast<std::size_t>(std::ranges::count_if(stored, [](const auto& entry) { return !entry.second.empty(); }));
        if (nContexts >= CtxSettingsBase::kMaxContexts) {
            return std::format("refused context {}: the block already stores {} contexts, the cap kMaxContexts", ctx.context, CtxSettingsBase::kMaxContexts);
        }
        return std::nullopt;
    }
    if (!forLaterTime) {
        return std::nullopt;
    }
    const auto nPending = static_cast<std::size_t>(std::ranges::count_if(contextIt->second, [now](const SettingsBase::CtxSettingsPair& pair) { return pair.context.time > now; }));
    if (nPending >= CtxSettingsBase::kMaxPendingSets) {
        return std::format("refused set for context {}: it already holds {} sets for a later time, the cap kMaxPendingSets", ctx.context, CtxSettingsBase::kMaxPendingSets);
    }
    return std::nullopt;
}

/// each key of `values` with its value, as "key = value", joined by commas
[[nodiscard]] std::string describeValues(const property_map& values) {
    std::string described;
    for (const auto& [key, value] : values) {
        described += std::format("{}{} = {}", described.empty() ? "" : ", ", key, value);
    }
    return described;
}

[[nodiscard]] std::string describeThrown(const std::exception_ptr& thrown) {
    try {
        std::rethrow_exception(thrown);
    } catch (const gr::exception& e) {
        return e.message;
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "unknown error";
    }
}

[[nodiscard]] std::string joinRefusals(const std::string& first, const std::string& second) {
    if (first.empty() || second.empty()) {
        return first + second;
    }
    return std::format("{}; {}", first, second);
}
} // namespace

CtxSettingsBase::CtxSettingsBase(void* block, const settings::BlockDescriptor& descriptor) noexcept : _block(block), _descriptor(&descriptor) { _autoForwardParameters.insert(gr::tag::kDefaultTags.begin(), gr::tag::kDefaultTags.end()); }

// --- Simple accessors ---

const std::set<std::string>& CtxSettingsBase::writableMembers() const { return _descriptor->writableMembers; }

std::set<std::string> CtxSettingsBase::readableMembers() const {
    std::set<std::string> names;
    for (const settings::MemberDescriptor* member : _descriptor->readableMembers) {
        names.emplace(member->name);
    }
    return names;
}

bool CtxSettingsBase::changed() const noexcept { return gr::atomic_ref(_changed).load_acquire(); }
void CtxSettingsBase::setChanged(bool b) noexcept { gr::atomic_ref(_changed).store_release(b); }

void CtxSettingsBase::setInitBlockParameters(const property_map& parameters) { _initBlockParameters = parameters; }

const SettingsCtx& CtxSettingsBase::activeContext() const noexcept { return _activeCtx; }

const std::set<std::string>& CtxSettingsBase::autoForwardParameters() const noexcept { return _autoForwardParameters; }

void CtxSettingsBase::addAutoForwardParameters(std::set<std::string> parameterKeys) {
    // the per-work() read of this set is unlocked because the set is fixed for the duration of a
    // run, so a mutation while the block is running is refused rather than allowed to race
    if (_descriptor->hooks.blockState != nullptr && lifecycle::isActive(_descriptor->hooks.blockState(_block))) {
        std::println(stderr, "gr::Settings: addAutoForwardParameters() refused on a running block; the auto-forward set is fixed for the duration of a run");
        return;
    }
    std::lock_guard guard(_mutex);
    _autoForwardParameters.merge(parameterKeys);
}

property_map CtxSettingsBase::defaultParameters() const noexcept {
    std::lock_guard lg(_mutex);
    return _defaultParameters;
}

property_map CtxSettingsBase::activeParameters() const noexcept {
    std::lock_guard lg(_mutex);
    return _activeParameters;
}

// --- get() overloads ---

property_map CtxSettingsBase::get(std::span<const std::string> parameterKeys) const noexcept {
    std::lock_guard lg(_mutex);
    if (parameterKeys.empty()) {
        return _activeParameters;
    }
    property_map ret;
    for (const auto& key : parameterKeys) {
        if (_activeParameters.contains(convert_string_domain(key))) {
            ret.insert_or_assign(convert_string_domain(key), _activeParameters.at(convert_string_domain(key)));
        }
    }
    return ret;
}

std::optional<pmt::Value> CtxSettingsBase::get(const std::string& parameterKey) const noexcept {
    auto res = get(std::array<std::string, 1>({parameterKey}));
    auto it  = res.find(convert_string_domain(parameterKey));
    if (it != res.end()) {
        return it->second;
    } else {
        return std::nullopt;
    }
}

// --- getStored() overloads ---

std::optional<property_map> CtxSettingsBase::getStored(std::span<const std::string> parameterKeys, SettingsCtx ctx) const noexcept {
    std::lock_guard lg(_mutex);
    if (ctx.time == 0ULL) {
        ctx.time = settings::convertTimePointToUint64Ns(std::chrono::system_clock::now());
    }
#ifdef __EMSCRIPTEN__
    ctx.time += _timePrecisionTolerance;
#endif
    std::optional<property_map> allBestMatchParameters = this->getBestMatchStoredParameters(ctx);

    if (allBestMatchParameters == std::nullopt) {
        return std::nullopt;
    }

    if (parameterKeys.empty()) {
        return allBestMatchParameters;
    }
    property_map ret;
    for (const auto& key : parameterKeys) {
        if (allBestMatchParameters->contains(convert_string_domain(key))) {
            ret.insert_or_assign(convert_string_domain(key), allBestMatchParameters->at(convert_string_domain(key)));
        }
    }
    return ret;
}

std::optional<pmt::Value> CtxSettingsBase::getStored(const std::string& parameterKey, SettingsCtx ctx) const noexcept {
    auto res = getStored(std::array<std::string, 1>({parameterKey}), ctx);

    if (res.has_value() && res->contains(convert_string_domain(parameterKey))) {
        return res->at(convert_string_domain(parameterKey));
    } else {
        return std::nullopt;
    }
}

// --- Remaining getters ---

gr::Size_t CtxSettingsBase::getNStoredParameters() const noexcept {
    std::lock_guard lg(_mutex);
    gr::Size_t      nParameters{0};
    for (const auto& stored : _storedParameters) {
        nParameters += static_cast<gr::Size_t>(stored.second.size());
    }
    return nParameters;
}

gr::Size_t CtxSettingsBase::getNAutoUpdateParameters() const noexcept {
    std::lock_guard lg(_mutex);
    return static_cast<gr::Size_t>(_autoUpdateParameters.size());
}

std::map<pmt::Value, std::vector<SettingsBase::CtxSettingsPair>, settings::PMTCompare> CtxSettingsBase::getStoredAll() const noexcept {
    std::lock_guard lg(_mutex);
    return _storedParameters;
}

property_map CtxSettingsBase::stagedParameters() const {
    std::lock_guard lg(_mutex);
    return _stagedParameters;
}

std::set<std::string> CtxSettingsBase::autoUpdateParameters(SettingsCtx ctx) noexcept {
    std::lock_guard lg(_mutex);
    const auto      bestMatchSettingsCtx = findBestMatchSettingsCtx(ctx);
    return bestMatchSettingsCtx == std::nullopt ? std::set<std::string>() : _autoUpdateParameters[bestMatchSettingsCtx.value()];
}

// --- setStaged() ---

property_map CtxSettingsBase::setStaged(const property_map& parameters) {
    std::lock_guard lg(_mutex);
    return setStagedImpl(parameters);
}

// re-applying a value the block already holds must cost nothing, so it is never staged
bool CtxSettingsBase::isActiveValueImpl(const std::pmr::string& key, const pmt::Value& value) const {
    const auto it = _activeParameters.find(key);
    return it != _activeParameters.end() && it->second == value;
}

// --- Context management ---

std::optional<SettingsCtx> CtxSettingsBase::activateContext(SettingsCtx ctx) {
    std::lock_guard lg(_mutex);
    return activateContextImpl(ctx);
}

std::optional<SettingsCtx> CtxSettingsBase::activateContextImpl(SettingsCtx ctx) {
    if (ctx.time == 0ULL) {
        ctx.time = settings::convertTimePointToUint64Ns(std::chrono::system_clock::now());
#ifdef __EMSCRIPTEN__
        ctx.time += _timePrecisionTolerance;
#endif
    }

    const std::optional<SettingsCtx> bestMatchSettingsCtx = findBestMatchSettingsCtx(ctx);
    if (!bestMatchSettingsCtx || bestMatchSettingsCtx == _activeCtx) {
        return bestMatchSettingsCtx;
    }

    if (bestMatchSettingsCtx.value().context == _activeCtx.context) {
        std::optional<property_map> parameters = getBestMatchStoredParameters(ctx);
        if (parameters) {
            if (!_autoUpdateParameters.contains(bestMatchSettingsCtx.value())) {
                _autoUpdateParameters[bestMatchSettingsCtx.value()] = getBestMatchAutoUpdateParameters(bestMatchSettingsCtx.value()).value_or(_descriptor->writableMembers);
            }
            const std::set<std::string>& currentAutoUpdateParams = _autoUpdateParameters.at(bestMatchSettingsCtx.value());

            property_map notAutoUpdateParams;
            for (const auto& pair : parameters.value()) {
                if (!currentAutoUpdateParams.contains(std::string(pair.first))) {
                    notAutoUpdateParams.insert(pair);
                }
            }

            std::ignore = setStagedImpl(std::move(notAutoUpdateParams));
            _activeCtx  = bestMatchSettingsCtx.value();
            setChanged(true);
        }
    } else {
        std::optional<property_map> _parameters = getBestMatchStoredParameters(ctx);
        if (_parameters) {
            auto& parameters = *_parameters;
            _stagedParameters.insert(parameters.begin(), parameters.end());
            if (!_contextActivated) {
                _contextBeforeActivation = _activeCtx;
            }
            _activeCtx = bestMatchSettingsCtx.value();
            // moving to another context is an event of its own: the next apply reports it whether or not the
            // context's parameters differ from the ones the block already holds
            _contextActivated = true;
            setChanged(true);
        } else {
            return std::nullopt;
        }
    }

    return bestMatchSettingsCtx;
}

bool CtxSettingsBase::removeContext(SettingsCtx ctx) {
    std::lock_guard lg(_mutex);
    return removeContextImpl(ctx);
}

bool CtxSettingsBase::removeContextImpl(SettingsCtx ctx) {
    auto str = ctx.context.value_or(std::string_view{});
    if (str.empty()) {
        return false; // Forbid removing default context
    }

    auto it = _storedParameters.find(ctx.context);
    if (it == _storedParameters.end()) {
        return false;
    }

    if (ctx.time == 0ULL) {
        ctx.time = settings::convertTimePointToUint64Ns(std::chrono::system_clock::now());
#ifdef __EMSCRIPTEN__
        ctx.time += _timePrecisionTolerance;
#endif
    }

    std::vector<CtxSettingsPair>& vec     = it->second;
    auto                          exactIt = std::find_if(vec.begin(), vec.end(), [&ctx](const auto& pair) { return pair.context.time == ctx.time; });

    if (exactIt == vec.end()) {
        return false;
    }
    _autoUpdateParameters.erase(exactIt->context);
    const bool removesEarlierContext = _contextBeforeActivation == exactIt->context;
    vec.erase(exactIt);

    if (vec.empty()) {
        _storedParameters.erase(ctx.context);
    }
    if (removesEarlierContext) {
        // a refusal falls back to the default context, as removing the active context does
        _contextBeforeActivation = findCurrentDefaultCtx();
    }

    if (_activeCtx.context == ctx.context) {
        std::ignore = activateContextImpl({}); // Activate default context
        _contextBeforeActivation.reset();      // a refusal keeps the default context
    }

    return true;
}

// --- assignFrom ---

void CtxSettingsBase::assignFrom(const CtxSettingsBase& other) {
    std::scoped_lock lock(_mutex, other._mutex);
    gr::atomic_ref(_changed).store_release(gr::atomic_ref(other._changed).load_acquire());
    _storedParameters        = other._storedParameters;
    _defaultParameters       = other._defaultParameters;
    _initBlockParameters     = other._initBlockParameters;
    _autoUpdateParameters    = other._autoUpdateParameters;
    _autoForwardParameters   = other._autoForwardParameters;
    _matchPred               = other._matchPred;
    _activeCtx               = other._activeCtx;
    _contextActivated        = other._contextActivated;
    _contextBeforeActivation = other._contextBeforeActivation;
    _autoUpdateBeforeReset   = other._autoUpdateBeforeReset;
    _stagedParameters        = other._stagedParameters;
    _taggedKeys              = other._taggedKeys;
    _tagCouplings            = other._tagCouplings;
    _activeParameters        = other._activeParameters;
}

void CtxSettingsBase::assignFrom(CtxSettingsBase&& other) noexcept {
    std::scoped_lock lock(_mutex, other._mutex);
    gr::atomic_ref(_changed).store_release(gr::atomic_ref(other._changed).load_acquire());
    _storedParameters        = std::move(other._storedParameters);
    _defaultParameters       = std::move(other._defaultParameters);
    _initBlockParameters     = std::move(other._initBlockParameters);
    _autoUpdateParameters    = std::move(other._autoUpdateParameters);
    _autoForwardParameters   = std::move(other._autoForwardParameters);
    _matchPred               = std::exchange(other._matchPred, settings::nullMatchPred);
    _activeCtx               = std::exchange(other._activeCtx, {});
    _contextActivated        = std::exchange(other._contextActivated, false);
    _contextBeforeActivation = std::exchange(other._contextBeforeActivation, std::nullopt);
    _autoUpdateBeforeReset   = std::exchange(other._autoUpdateBeforeReset, std::nullopt);
    _stagedParameters        = std::move(other._stagedParameters);
    _taggedKeys              = std::move(other._taggedKeys);
    _tagCouplings            = std::move(other._tagCouplings);
    _activeParameters        = std::move(other._activeParameters);
}

// --- Private helpers: match/search ---

std::optional<pmt::Value> CtxSettingsBase::findBestMatchCtx(const pmt::Value& contextToSearch) const {
    if (_storedParameters.empty()) {
        return std::nullopt;
    }

    // exact match
    if (_storedParameters.find(contextToSearch) != _storedParameters.end()) {
        return contextToSearch;
    }

    // retry with increasing attempt counts; bounded because a user MatchPredicate that keeps returning
    // 'no match yet' would otherwise spin forever while holding _mutex
    constexpr std::size_t kMaxMatchAttempts = 64UZ;
    for (std::size_t attempt = 0UZ; attempt < kMaxMatchAttempts; ++attempt) {
        for (const auto& i : _storedParameters) {
            const auto matches = _matchPred(i.first, contextToSearch, attempt);
            if (!matches) {
                return std::nullopt;
            } else if (*matches) {
                return i.first; // return the best matched SettingsCtx.context
            }
        }
    }
    return std::nullopt;
}

std::optional<SettingsCtx> CtxSettingsBase::findBestMatchSettingsCtx(const SettingsCtx& ctx) const {
    const auto bestMatchCtx = findBestMatchCtx(ctx.context);
    if (bestMatchCtx == std::nullopt) {
        return std::nullopt;
    }
    const auto& vec = _storedParameters[bestMatchCtx.value()];
    if (vec.empty()) {
        return std::nullopt;
    }
    if (ctx.time == 0ULL || vec.back().context.time <= ctx.time) {
        return vec.back().context;
    } else {
        auto lower = std::ranges::lower_bound(vec, ctx.time, {}, [](const auto& a) { return a.context.time; });
        if (lower == vec.end()) {
            return vec.back().context;
        } else {
            if (lower->context.time == ctx.time) {
                return lower->context;
            } else if (lower != vec.begin()) {
                --lower;
                return lower->context;
            }
        }
    }
    return std::nullopt;
}

std::optional<SettingsCtx> CtxSettingsBase::findCurrentDefaultCtx() const {
    SettingsCtx ctx{};
    ctx.time = settings::convertTimePointToUint64Ns(std::chrono::system_clock::now());
#ifdef __EMSCRIPTEN__
    ctx.time += _timePrecisionTolerance;
#endif
    return findBestMatchSettingsCtx(ctx);
}

std::optional<property_map> CtxSettingsBase::getBestMatchStoredParameters(const SettingsCtx& ctx) const {
    const auto bestMatchSettingsCtx = findBestMatchSettingsCtx(ctx);
    if (bestMatchSettingsCtx == std::nullopt) {
        return std::nullopt;
    }
    const auto& vec        = _storedParameters[bestMatchSettingsCtx.value().context];
    const auto  parameters = std::ranges::find_if(vec, [&](const CtxSettingsPair& contextSettings) { return contextSettings.context == bestMatchSettingsCtx.value(); });

    return parameters != vec.end() ? std::optional(parameters->settings) : std::nullopt;
}

std::optional<std::set<std::string>> CtxSettingsBase::getBestMatchAutoUpdateParameters(const SettingsCtx& ctx) const {
    const auto bestMatchSettingsCtx = findBestMatchSettingsCtx(ctx);
    if (bestMatchSettingsCtx == std::nullopt || !_autoUpdateParameters.contains(bestMatchSettingsCtx.value())) {
        return std::nullopt;
    } else {
        return _autoUpdateParameters.at(bestMatchSettingsCtx.value());
    }
}

// --- Private helpers: storage/expiry ---

void CtxSettingsBase::resolveDuplicateTimestamp(SettingsCtx& ctx) {
    const auto vecIt = _storedParameters.find(ctx.context);
    if (vecIt == _storedParameters.end() || vecIt->second.empty()) {
        return;
    }
    const auto&       vec       = vecIt->second;
    const std::size_t tolerance = 1000; // ns
    // find the last context in sorted vector such that `ctx.time <= ctxToFind <= ctx.time + tolerance`
    const auto lower = std::ranges::lower_bound(vec, ctx.time, {}, [](const auto& elem) { return elem.context.time; });
    const auto upper = std::ranges::upper_bound(vec, ctx.time + tolerance, {}, [](const auto& elem) { return elem.context.time; });
    if (lower != upper && lower != vec.end()) {
        ctx.time = (*(upper - 1)).context.time + 1;
    }
}

void CtxSettingsBase::addStoredParameters(const property_map& newParameters, const SettingsCtx& ctx) {
    if (!_autoUpdateParameters.contains(ctx)) {
        _autoUpdateParameters[ctx] = getBestMatchAutoUpdateParameters(ctx).value_or(_descriptor->writableMembers);
    }

    std::vector<CtxSettingsPair>& sortedVectorForContext = _storedParameters[ctx.context];
    // binary search and merge-sort
    auto it = std::ranges::lower_bound(sortedVectorForContext, ctx.time, std::less<>{}, [](const auto& pair) { return pair.context.time; });
    sortedVectorForContext.insert(it, {ctx, newParameters});
}

void CtxSettingsBase::removeExpiredStoredParameters() {
    const auto removeFromAutoUpdateParameters = [this](const auto& begin, const auto& end) {
        for (auto it = begin; it != end; it++) {
            _autoUpdateParameters.erase(it->context);
        }
    };
    std::uint64_t now = settings::convertTimePointToUint64Ns(std::chrono::system_clock::now());
#ifdef __EMSCRIPTEN__
    now += _timePrecisionTolerance;
#endif
    for (auto& [ctx, vec] : _storedParameters) {
        // remove all expired parameters
        if (expiry_time != std::numeric_limits<std::uint64_t>::max()) {
            const auto isExpired = [&](const auto& elem) { return elem.context.time + expiry_time <= now; };
            for (const auto& elem : vec) { // collect before remove_if leaves the tail moved-from
                if (isExpired(elem)) {
                    _autoUpdateParameters.erase(elem.context);
                }
            }
            const auto [first, last] = std::ranges::remove_if(vec, isExpired);
            vec.erase(first, last);
        }

        if (vec.empty()) {
            continue;
        }
        // always keep at least one past parameter set
        auto lower = std::ranges::lower_bound(vec, now, {}, [](const auto& elem) { return elem.context.time; });
        if (lower == vec.end()) {
            removeFromAutoUpdateParameters(vec.begin(), vec.end() - 1);
            vec.erase(vec.begin(), vec.end() - 1);
        } else {
            if (lower->context.time == now) {
                removeFromAutoUpdateParameters(vec.begin(), lower);
                vec.erase(vec.begin(), lower);
            } else if (lower != vec.begin() && lower - 1 != vec.begin()) {
                removeFromAutoUpdateParameters(vec.begin(), lower - 1);
                vec.erase(vec.begin(), lower - 1);
            }
        }
    }
}

// --- Private helpers: tag parsing ---

std::optional<std::string> CtxSettingsBase::contextInTag(const Tag& tag) const {
    if (tag.map.contains(gr::tag::CONTEXT.key())) {
        const pmt::Value& ctxInfo = tag.map.at(gr::tag::CONTEXT.key());
        auto              result  = ctxInfo.value_or(std::string_view{});
        if (result.data() != nullptr) {
            return {std::string(result)};
        }
    }
    return std::nullopt;
}

std::optional<std::uint64_t> CtxSettingsBase::contextTimeInTag(const Tag& tag) const {
    if (tag.map.contains(gr::tag::CONTEXT_TIME.key())) {
        const pmt::Value& pmtTimeUtcNs = tag.map.at(gr::tag::CONTEXT_TIME.key());
        auto              result       = pmt::convert_safely<std::uint64_t>(pmtTimeUtcNs);
        if (result) {
            return *result;
        }
    }
    return std::nullopt;
}

std::optional<SettingsCtx> CtxSettingsBase::createSettingsCtxFromTag(const Tag& tag) const {
    // a tag without gr:context selects no context, and one without gr:ctx_time selects the context's set for now

    if (auto ctxValue = contextInTag(tag); ctxValue.has_value()) {
        SettingsCtx ctx{};
        ctx.context = ctxValue.value();

        if (auto contextTime = contextTimeInTag(tag); contextTime.has_value()) {
            ctx.time = contextTime.value();
        }
        if (ctx.time == 0ULL) {
            ctx.time = settings::convertTimePointToUint64Ns(std::chrono::system_clock::now());
        }
        return ctx;
    } else {
        return std::nullopt;
    }
}

void CtxSettingsBase::init() {
    const settings::BlockHooks& hooks = _descriptor->hooks;

    if (hooks.reflectable && hooks.metaInformation != nullptr) {
        property_map& metaInformation = hooks.metaInformation(_block);

        if (hooks.blockDescription != nullptr) {
            metaInformation["description"] = std::string(hooks.blockDescription(_block));
        }

        // handle meta-information for UI and other non-processing-related purposes
        for (const settings::MemberDescriptor& member : hooks.members) {
            const std::string memberName = std::string(member.name);

            if (member.isEnum) {
                std::vector<std::string> enumValues;
                enumValues.reserve(member.enumValueNames.size());
                for (std::string_view enumValueName : member.enumValueNames) {
                    if (!enumValueName.empty()) {
                        enumValues.emplace_back(enumValueName);
                    }
                }
                metaInformation[convert_string_domain(memberName) + "::enum_values"] = enumValues;
                metaInformation[convert_string_domain(memberName) + "::enum_type"]   = member.enumTypeName();
            }

            if (member.isAnnotated) {
                metaInformation[convert_string_domain(memberName) + "::description"]   = std::string(member.description);
                metaInformation[convert_string_domain(memberName) + "::documentation"] = std::string(member.documentation);
                metaInformation[convert_string_domain(memberName) + "::unit"]          = std::string(member.unit);
                metaInformation[convert_string_domain(memberName) + "::visible"]       = member.visible;
            }
        }
    }

    storeDefaults();

    // a refusal names the block by the name the constructor's map gives. The apply assigns that name later.
    const auto             nameIt      = _initBlockParameters.find("name");
    const std::string_view initialName = nameIt != _initBlockParameters.end() ? nameIt->second.value_or(std::string_view{}) : std::string_view{};
    // a coupled setting that follows its key keeps following it after the constructor's map sets it
    std::vector<std::string> keepFollowing;
    const property_map       notSet = [this, initialName, &keepFollowing] {
        std::lock_guard             lg(_mutex);
        const std::set<std::string> following = getBestMatchAutoUpdateParameters(SettingsCtx{settings::convertTimePointToUint64Ns(std::chrono::system_clock::now()), std::string()}).value_or(_descriptor->writableMembers);
        for (const auto& [setting, _] : _tagCouplings) {
            if (_initBlockParameters.contains(std::string_view(setting)) && following.contains(setting)) {
                keepFollowing.push_back(setting);
            }
        }
        return setImpl(_initBlockParameters, {}, initialName);
    }();
    if (!notSet.empty()) {
        throw gr::exception(std::format("settings could not be applied: {}", notSet));
    }

    if (const auto failed = activateContext(); failed == std::nullopt) {
        throw gr::exception("Settings for context could not be activated");
    }
    if (!keepFollowing.empty()) {
        // the settings follow again in the default context's set, which the constructor's map wrote; another context
        // keeps its own set
        std::lock_guard                  lg(_mutex);
        const std::optional<SettingsCtx> initCtx = findBestMatchSettingsCtx(SettingsCtx{settings::convertTimePointToUint64Ns(std::chrono::system_clock::now()), std::string()});
        if (initCtx.has_value() && _autoUpdateParameters.contains(*initCtx)) {
            _autoUpdateParameters.at(*initCtx).insert(keepFollowing.begin(), keepFollowing.end());
        }
    }
}

property_map CtxSettingsBase::set(const property_map& parameters, SettingsCtx ctx) {
    std::lock_guard lg(_mutex);
    return setImpl(parameters, ctx);
}

property_map CtxSettingsBase::setImpl(const property_map& parameters, SettingsCtx ctx, std::string_view blockName) {
    const settings::BlockHooks& hooks = _descriptor->hooks;

    property_map ret;
    if (hooks.reflectable) {
        const std::uint64_t now = settings::convertTimePointToUint64Ns(std::chrono::system_clock::now());
        if (ctx.time == 0ULL) {
            ctx.time = now;
        }
        const bool forLaterTime = ctx.time > now;
#ifdef __EMSCRIPTEN__
        resolveDuplicateTimestamp(ctx);
#endif
        if (std::optional<std::string> refusal = refuseStoredSet(_storedParameters, ctx, forLaterTime, now)) {
            throw gr::exception(*refusal);
        }
        // initialize with empty property_map when best match parameters not found
        property_map newParameters = getBestMatchStoredParameters(ctx).value_or(_defaultParameters);
        // the context keeps its keys that follow tags until every value of the map is taken
        std::set<std::string> currentAutoUpdateParameters = _autoUpdateParameters.contains(ctx) ? _autoUpdateParameters.at(ctx) : getBestMatchAutoUpdateParameters(ctx).value_or(_descriptor->writableMembers);

        for (const auto& [key, value] : parameters) {
            if (value.is_monostate()) {
                continue;
            }

            auto it = _descriptor->writableByName.find(key);
            if (it != _descriptor->writableByName.end()) {
                if (auto error = it->second->setParameter(key, value, newParameters)) {
                    throw gr::exception(*error);
                }
                if (it->second->refuseByLimits != nullptr && it->second->refuseByLimits(key, value).has_value()) {
                    throw gr::exception(describeRefusal(key, value, blockName));
                }
                // Remove from auto-update set if present
                if (auto autoIt = currentAutoUpdateParameters.find(std::string(key)); autoIt != currentAutoUpdateParameters.end()) {
                    currentAutoUpdateParameters.erase(autoIt);
                }
            } else {
                ret.insert_or_assign(key, value);
            }
        }
        _autoUpdateParameters[ctx] = std::move(currentAutoUpdateParameters);
        addStoredParameters(newParameters, ctx);
        removeExpiredStoredParameters();
    }

    // copy items that could not be matched to the node's meta_information map (if available)
    if (hooks.metaInformation != nullptr) {
        updateMaps(ret, hooks.metaInformation(_block));
    }

    return ret; // N.B. returns those <key:value> parameters that could not be set
}

property_map CtxSettingsBase::setStagedImpl(const property_map& parameters) {
    property_map ret;
    if (_descriptor->hooks.reflectable) {
        // a value that does not convert throws before any value is staged
        property_map converted;
        for (const auto& [key, value] : parameters) {
            auto it = _descriptor->writableByName.find(key);
            if (it != _descriptor->writableByName.end()) {
                if (auto error = it->second->setParameter(key, value, converted)) {
                    throw gr::exception(*error);
                }
            } else {
                ret.insert_or_assign(key, value);
            }
        }
        for (auto& [key, value] : converted) {
            if (const auto tagged = _taggedKeys.find(std::string_view(key)); tagged != _taggedKeys.end()) {
                _taggedKeys.erase(tagged);
            }
            _stagedParameters.insert_or_assign(key, std::move(value));
        }
    }
    if (!_stagedParameters.empty()) {
        setChanged(true);
    }
    return ret;
}

void CtxSettingsBase::storeDefaults() { storeCurrentParameters(_defaultParameters); }

void CtxSettingsBase::resetDefaults() {
    std::unique_lock          lock(_mutex);
    const ResetDefaultsResult reset   = resetDefaultsImpl(lock);
    const std::string         refusal = describeRefusals(reset.failedParameters);
    lock.unlock(); // the block's reset() may read or write settings()
    if (_descriptor->hooks.reset != nullptr && !reset.refused) {
        _descriptor->hooks.reset(_block);
    }
    if (!reset.failedParameters.empty()) {
        throw gr::exception(refusal);
    }
}

CtxSettingsBase::ResetDefaultsResult CtxSettingsBase::resetDefaultsImpl(std::unique_lock<std::mutex>& reentrantLock) {
    if (property_map refused = stageDefaultsImpl(); !refused.empty()) {
        return {.failedParameters = std::move(refused), .refused = true};
    }
    // a reset_default tag staged since the last apply is served by this reset
    _stagedParameters.erase(std::pmr::string(gr::tag::RESET_DEFAULTS.key()));
    ApplyStagedParametersResult defaults = applyStagedParametersImpl(reentrantLock);

    removeExpiredStoredParameters();
    return {.failedParameters = std::move(defaults.failedParameters), .refused = false};
}

property_map CtxSettingsBase::stageDefaultsImpl() {
    // add default parameters to stored and stage them
    auto ctx = SettingsCtx{settings::convertTimePointToUint64Ns(std::chrono::system_clock::now()), std::string()};
#ifdef __EMSCRIPTEN__
    resolveDuplicateTimestamp(ctx);
#endif
    // the activation below stages every default when another context is active, otherwise the defaults outside the
    // context's auto-update set. A default the limits refuse stops the reset before anything is stored or applied.
    const bool                  otherContextActive = _activeCtx.context != ctx.context;
    const std::set<std::string> autoUpdate         = getBestMatchAutoUpdateParameters(ctx).value_or(_descriptor->writableMembers);
    property_map                refused;
    for (const auto& [key, value] : _defaultParameters) {
        const auto it = _descriptor->writableByName.find(key);
        if (it == _descriptor->writableByName.end() || it->second->refuseByLimits == nullptr || (!otherContextActive && autoUpdate.contains(std::string(key)))) {
            continue;
        }
        if (it->second->refuseByLimits(key, value).has_value()) {
            refused.insert_or_assign(key, value);
        }
    }
    if (!refused.empty()) {
        return refused;
    }

    addStoredParameters(_defaultParameters, ctx);
    std::ignore = activateContextImpl({});
    // kept for a refused reset; taken after the new set's entry is filled from the set it came from
    _autoUpdateBeforeReset = _autoUpdateParameters;
    // every coupled setting follows its tag key again in every context, whatever set it
    _autoUpdateParameters[ctx] = _descriptor->writableMembers;
    for (auto& [_, autoUpdateKeys] : _autoUpdateParameters) {
        autoUpdateKeys = _descriptor->writableMembers;
    }
    return {};
}

void CtxSettingsBase::autoUpdate(const Tag& tag) {
    if (!_descriptor->hooks.reflectable) {
        return;
    }
    std::lock_guard lg(_mutex);
    const auto      tagCtx      = createSettingsCtxFromTag(tag);
    const auto      previousCtx = _activeCtx; // capture before activateContext may change it

    SettingsCtx ctx;
    if (tagCtx != std::nullopt) {
        const auto bestMatchSettingsCtx = activateContextImpl(tagCtx.value());
        if (bestMatchSettingsCtx == std::nullopt) {
            ctx = _activeCtx;
        } else {
            ctx = bestMatchSettingsCtx.value();
        }
    } else {
        ctx = _activeCtx;
    }

    const bool activeCtxChanged = previousCtx != ctx;

    // only a stored set keeps an entry: pruning a set erases its entry, and the active set can be a pruned one
    const auto                   storedIt         = _storedParameters.find(ctx.context);
    const bool                   isStored         = storedIt != _storedParameters.end() && std::ranges::any_of(storedIt->second, [&ctx](const CtxSettingsPair& pair) { return pair.context == ctx; });
    const std::set<std::string>* autoUpdateParams = &_descriptor->writableMembers;
    if (isStored) {
        if (!_autoUpdateParameters.contains(ctx)) {
            // fuzzy-match auto-update parameters (exact lookup may fail due to timestamp mismatch)
            _autoUpdateParameters[ctx] = getBestMatchAutoUpdateParameters(ctx).value_or(_descriptor->writableMembers);
        }
        autoUpdateParams = &_autoUpdateParameters.at(ctx);
    } else {
        // an active set that a newer set of its context pruned is matched by no stored set; the context's present
        // set then holds the keys that follow tags
        std::optional<SettingsCtx> bestMatch = findBestMatchSettingsCtx(ctx);
        if (!bestMatch.has_value()) {
            std::uint64_t now = settings::convertTimePointToUint64Ns(std::chrono::system_clock::now());
#ifdef __EMSCRIPTEN__
            now += _timePrecisionTolerance;
#endif
            bestMatch = findBestMatchSettingsCtx(SettingsCtx{.time = now, .context = ctx.context});
        }
        if (bestMatch.has_value() && _autoUpdateParameters.contains(*bestMatch)) {
            autoUpdateParams = &_autoUpdateParameters.at(*bestMatch);
        }
    }

    const auto& parameters = tag.map;
    bool        wasChanged = false;
    for (const auto& [key, value] : parameters) {
        // the two keys are staged under their gr: key, which names no member
        const bool resetKey = key == gr::tag::RESET_DEFAULTS;
        if (resetKey || key == gr::tag::STORE_DEFAULTS) {
            _stagedParameters.insert_or_assign(std::pmr::string(resetKey ? gr::tag::RESET_DEFAULTS.key() : gr::tag::STORE_DEFAULTS.key()), value);
            wasChanged = true;
            continue;
        }
    }
    for (const auto& [setting, tagKey] : _tagCouplings) {
        const auto tagged = parameters.find(std::string_view(tagKey));
        const auto member = _descriptor->writableByName.find(std::string_view(setting));
        if (tagged == parameters.end() || member == _descriptor->writableByName.end()) {
            continue;
        }
        if (activeCtxChanged || !isActiveValueImpl(std::pmr::string(setting), tagged->second)) {
            if (member->second->autoUpdate(setting, tagged->second, *autoUpdateParams, _stagedParameters)) {
                _taggedKeys.emplace(setting);
                wasChanged = true;
            }
        }
    }

    if (tagCtx == std::nullopt && !wasChanged && _stagedParameters.empty()) {
        setChanged(false);
    } else if (activeCtxChanged || wasChanged) {
        setChanged(true);
    }
}

namespace {
/// why `key` cannot be a coupling's tag key, or nothing for a key that can
[[nodiscard]] std::optional<std::string_view> refuseTagKey(std::string_view key) {
    const bool printable = std::ranges::all_of(key, [](char c) { return static_cast<unsigned char>(c) > 0x20U && c != 0x7F; });
    if (key.starts_with(':') || gr::tag::nameOf(key).empty() || !printable) {
        return "the key is not domain:name with a name and no space";
    }
    if (std::ranges::contains(gr::tag::kControlKeys, key)) {
        return "the framework acts on this key in every block";
    }
    return std::nullopt;
}
} // namespace

void CtxSettingsBase::setTagCouplings(const property_map& couplings) {
    std::lock_guard lg(_mutex);
    auto            table  = _tagCouplings;
    const auto      refuse = [this](std::string_view setting, const pmt::Value& key, std::string_view reason) {
        const std::string_view blockName = _descriptor->hooks.blockName != nullptr ? _descriptor->hooks.blockName(_block) : std::string_view{};
        return gr::exception(std::format("block '{}' refuses to couple {} to the tag key {}: {}", blockName, setting, key, reason));
    };
    std::vector<std::string> following;
    for (const auto& [setting, key] : couplings) {
        if (!_descriptor->writableByName.contains(std::string_view(setting))) {
            throw refuse(setting, key, "the block has no writable setting of that name");
        }
        if (!key.is_string()) {
            throw refuse(setting, key, "the key is not a string");
        }
        const std::string_view name    = setting;
        const std::string_view spelled = key.value_or(std::string_view{});
        if (spelled.empty()) {
            if (const auto coupled = table.find(name); coupled != table.end()) {
                table.erase(coupled);
            }
            continue;
        }
        const std::string_view canonical = gr::tag::canonicalKey(spelled);
        if (const std::optional<std::string_view> reason = refuseTagKey(canonical)) {
            throw refuse(setting, key, *reason);
        }
        table.insert_or_assign(std::string(name), std::string(canonical));
        following.emplace_back(name);
    }
    _tagCouplings = std::move(table);
    for (auto& [ctx, autoUpdateKeys] : _autoUpdateParameters) {
        autoUpdateKeys.insert(following.begin(), following.end());
    }
}

property_map CtxSettingsBase::tagCouplings() const {
    std::lock_guard lg(_mutex);
    property_map    table;
    for (const auto& [setting, key] : _tagCouplings) {
        table.insert_or_assign(std::pmr::string(setting), pmt::Value(key));
    }
    return table;
}

ApplyStagedParametersResult CtxSettingsBase::applyStagedParameters() {
    std::unique_lock lock(_mutex);
    return applyStagedParametersImpl(lock);
}

ApplyStagedParametersResult CtxSettingsBase::applyStagedParametersImpl(std::unique_lock<std::mutex>& reentrantLock) {
    const settings::BlockHooks& hooks = _descriptor->hooks;

    // one apply consumes one activation, so an event is reported once and does not reach a later batch
    bool                       contextActivated = std::exchange(_contextActivated, false);
    std::optional<SettingsCtx> contextBefore    = std::exchange(_contextBeforeActivation, std::nullopt);
    SettingsCtx                batchCtx         = _activeCtx; // a callback may activate another context before it refuses
    // set when resetDefaults() staged the defaults this apply applies
    std::optional<std::map<SettingsCtx, std::set<std::string>>> autoUpdateBefore = std::exchange(_autoUpdateBeforeReset, std::nullopt);

    ApplyStagedParametersResult result;
    if (hooks.reflectable) {
        // prepare old settings if required
        const bool   hasSettingsChangedCallback = hooks.settingsChanged != nullptr;
        property_map oldSettings;
        if (hasSettingsChangedCallback) {
            storeCurrentParameters(oldSettings);
        }

        // The batch is moved out of the shared map before anything below can release the lock. A
        // setStaged() call that arrives during the settingsChanged() window then stages for the
        // next apply instead of being cleared with this one, and a concurrent apply takes an empty
        // map rather than repeating this batch.
        const property_map                       batch      = std::exchange(_stagedParameters, {});
        const std::set<std::string, std::less<>> taggedKeys = std::exchange(_taggedKeys, {});

        // a reset stages the defaults it restores, and the batch's own values apply over them as one change
        const bool   resetRequested = batch.contains(gr::tag::RESET_DEFAULTS.key());
        bool         resetRefused   = false;
        property_map restored;
        if (resetRequested) {
            result.failedParameters = stageDefaultsImpl();
            resetRefused            = !result.failedParameters.empty();
            if (!resetRefused) {
                restored = std::exchange(_stagedParameters, {});
                for (const auto& [key, value] : batch) {
                    restored.insert_or_assign(key, value);
                }
                contextActivated = std::exchange(_contextActivated, false) || contextActivated;
                // a refusal of the batch returns the block to the context active before the batch
                if (std::optional<SettingsCtx> beforeReset = std::exchange(_contextBeforeActivation, std::nullopt); !contextBefore.has_value()) {
                    contextBefore = beforeReset;
                }
                batchCtx         = _activeCtx;
                autoUpdateBefore = std::exchange(_autoUpdateBeforeReset, std::nullopt);
                removeExpiredStoredParameters();
            }
        }
        const property_map& toApply = resetRequested && !resetRefused ? restored : batch;

        property_map staged;
        for (const auto& [key, stagedValue] : toApply) {
            auto it = _descriptor->writableByName.find(key);
            if (it != _descriptor->writableByName.end()) {
                if (!it->second->applyStaged(it->second->address(_block), key, stagedValue, result.appliedParameters, staged, hasSettingsChangedCallback)) {
                    result.failedParameters.insert_or_assign(key, stagedValue); // rejected: must not reach downstream blocks
                    continue;
                }
            }
        }

        // a value the block already holds moves nothing: every key `newSettings` names is a key whose
        // value in `oldSettings` differs, and a batch that moves none reports no change at all
        std::erase_if(staged, [&oldSettings](const auto& entry) {
            const auto previous = oldSettings.find(entry.first);
            return previous != oldSettings.end() && previous->second == entry.second;
        });

        // an activation is exempt from that rule: the newly active context is named after the erase, so a block
        // sees the event even when the two contexts carry the same values
        if (contextActivated && hasSettingsChangedCallback) {
            staged.insert_or_assign(std::pmr::string(settings::kContextField), _activeCtx.context);
        }

        updateActiveParametersImpl();

        std::string  callbackRefusal;
        property_map callbackRefused;
        // invoke user-callback function if staged is not empty; oldSettings, staged and result are local
        // copies, so the callback may read or write settings() without _mutex being held across it
        if (!staged.empty()) {
            reentrantLock.unlock();
            // the callback edits a copy: a refusal writes back and names the values in `staged`
            property_map       newSettings = staged;
            std::exception_ptr refusal;
            try {
                hooks.settingsChanged(_block, oldSettings, newSettings, result.forwardParameters);
            } catch (...) {
                refusal = std::current_exception();
            }
            reentrantLock.lock();
            if (refusal != nullptr) {
                restoreRefusedImpl(staged, oldSettings, batchCtx, contextActivated);
                if (contextBefore.has_value()) {
                    restoreContextImpl(*contextBefore, batchCtx);
                }
                // a refused reset puts back the keys that followed tags in every set the snapshot holds. A set that
                // pruning erased stays erased, and a set stored during the callback keeps its keys.
                if (autoUpdateBefore.has_value()) {
                    for (auto& [ctx, autoUpdateKeys] : _autoUpdateParameters) {
                        if (const auto before = autoUpdateBefore->find(ctx); before != autoUpdateBefore->end()) {
                            autoUpdateKeys = before->second;
                        }
                    }
                }
                updateActiveParametersImpl();
                // a refusal of values that tags alone staged is reported with the refusals of tag values
                const bool taggedOnly = std::ranges::all_of(staged, [&taggedKeys](const auto& entry) { return taggedKeys.contains(std::string_view(entry.first)); });
                if (!taggedOnly) {
                    std::rethrow_exception(refusal);
                }
                callbackRefusal = std::format("block '{}' refuses {}: {}", hooks.blockName != nullptr ? hooks.blockName(_block) : std::string_view{}, describeValues(staged), describeThrown(refusal));
                for (const auto& [key, value] : staged) {
                    result.appliedParameters.erase(key);
                }
                // the map holds only what the refusing call wrote, and the whole batch is written back
                result.forwardParameters.clear();
                callbackRefused = staged;
            }
        }

        updateActiveParametersImpl();
        result.refusal = describeRefusals(result.failedParameters);
        if (!result.failedParameters.empty()) {
            property_map tagged;
            property_map untagged;
            for (const auto& [key, value] : result.failedParameters) {
                (taggedKeys.contains(std::string_view(key)) ? tagged : untagged).insert_or_assign(key, value);
            }
            result.tagRefusal      = describeRefusals(tagged);
            result.untaggedRefusal = describeRefusals(untagged);
        }
        if (!callbackRefusal.empty()) {
            result.refusal    = joinRefusals(result.refusal, callbackRefusal);
            result.tagRefusal = joinRefusals(result.tagRefusal, callbackRefusal);
            result.failedParameters.insert(callbackRefused.begin(), callbackRefused.end());
        }

        if (batch.contains(gr::tag::STORE_DEFAULTS.key())) {
            storeDefaults();
        }

        // a reset the limits refuse stores and applies no default and skips the block's reset(). The batch is
        // applied in full when reset() runs, and reset() may read or write settings() with _mutex released.
        if (hooks.reset != nullptr && resetRequested && !resetRefused) {
            reentrantLock.unlock();
            hooks.reset(_block);
            reentrantLock.lock();
        }
    } else {
        _stagedParameters.clear(); // a block with no reflectable members cannot apply these
    }
    // anything staged while the lock was released is work for the next apply, so the map reports
    // unchanged only when it is empty
    gr::atomic_ref(_changed).store_release(!_stagedParameters.empty());
    return result;
}

void CtxSettingsBase::updateActiveParameters() noexcept {
    if (!_descriptor->hooks.reflectable) {
        return;
    }
    std::lock_guard lg(_mutex);
    updateActiveParametersImpl();
}

void CtxSettingsBase::updateActiveParametersImpl() noexcept {
    for (const settings::MemberDescriptor* member : _descriptor->readableMembers) {
        member->readParameter(member->address(_block), member->name, _activeParameters);
    }
}

std::size_t CtxSettingsBase::updateActiveParameters(std::initializer_list<std::string_view> keys) noexcept {
    if (!_descriptor->hooks.reflectable) {
        return 0UZ;
    }
    std::lock_guard lg(_mutex);
    std::size_t     nCopied = 0UZ;
    for (const settings::MemberDescriptor* member : _descriptor->readableMembers) {
        if (std::ranges::contains(keys, member->name)) {
            member->readParameter(member->address(_block), member->name, _activeParameters);
            nCopied++;
        }
    }
    return nCopied;
}

void CtxSettingsBase::restoreRefusedImpl(const property_map& refused, const property_map& oldSettings, const SettingsCtx& batchCtx, bool contextActivated) {
    // a batch that activated a context leaves every stored set as it is
    property_map* batchStored = nullptr;
    if (const auto stored = _storedParameters.find(batchCtx.context); !contextActivated && stored != _storedParameters.end()) {
        const auto batchSet = std::ranges::find_if(stored->second, [&batchCtx](const CtxSettingsPair& pair) { return pair.context == batchCtx; });
        if (batchSet != stored->second.end()) {
            batchStored = &batchSet->settings;
        }
    }
    for (const auto& [key, refusedValue] : refused) {
        const auto member   = _descriptor->writableByName.find(key);
        const auto previous = oldSettings.find(key);
        if (member == _descriptor->writableByName.end() || previous == oldSettings.end()) {
            continue;
        }
        property_map applied;
        property_map forCallback;
        // a member whose limits refuse its previous value keeps the refused value, and the stored set stays as it is
        if (!member->second->applyStaged(member->second->address(_block), key, previous->second, applied, forCallback, false)) {
            continue;
        }
        if (batchStored != nullptr) {
            if (const auto storedValue = batchStored->find(key); storedValue != batchStored->end() && storedValue->second == refusedValue) {
                storedValue->second = previous->second;
            }
        }
    }
}

void CtxSettingsBase::restoreContextImpl(const SettingsCtx& earlier, const SettingsCtx& refused) {
    // a removal of the earlier context's last stored set leaves the default context in its place
    const std::optional<SettingsCtx> restored = _storedParameters.contains(earlier.context) ? std::optional(earlier) : findCurrentDefaultCtx();
    // a context another call activated while the callback ran stays active
    if (_activeCtx == refused && restored.has_value()) {
        _activeCtx = *restored;
    }
    // that activation recorded the refused context, which a refusal of its own batch must not make active
    if (_contextBeforeActivation == refused) {
        _contextBeforeActivation = restored;
    }
}

void CtxSettingsBase::storeCurrentParameters(property_map& parameters) {
    if (!_descriptor->hooks.reflectable) {
        return;
    }
    for (const settings::MemberDescriptor* member : _descriptor->readableMembers) {
        member->readParameter(member->address(_block), member->name, parameters);
    }
}

std::string CtxSettingsBase::describeRefusal(std::string_view key, const pmt::Value& value, std::string_view blockName) const {
    std::string reason = "the value does not convert to the setting's type";
    if (const auto it = _descriptor->writableByName.find(key); it != _descriptor->writableByName.end() && it->second->refuseByLimits != nullptr) {
        if (std::optional<std::string> limits = it->second->refuseByLimits(key, value)) {
            reason = std::move(*limits);
        }
    }
    if (blockName.empty() && _descriptor->hooks.blockName != nullptr) {
        blockName = _descriptor->hooks.blockName(_block);
    }
    return std::format("block '{}' refuses {} = {}: {}", blockName, key, value, reason);
}

std::string CtxSettingsBase::describeRefusals(const property_map& refused) const {
    std::string described;
    for (const auto& [key, value] : refused) {
        described += std::format("{}{}", described.empty() ? "" : "; ", describeRefusal(key, value));
    }
    return described;
}

void CtxSettingsBase::loadParametersFromPropertyMap(const property_map& parameters, SettingsCtx ctx) {
    std::lock_guard lg(_mutex);
    property_map    newProperties;

    for (const auto& [key, value] : parameters) {
        if (_descriptor->writableByName.contains(key)) {
            newProperties[key] = value;
        } else {
            auto str = ctx.context.value_or(std::string_view{});
            if (str.empty() && _descriptor->hooks.metaInformation != nullptr) { // store meta_information only for default
                _descriptor->hooks.metaInformation(_block)[key] = value;
            }
        }
    }

    if (const property_map failed = setImpl(newProperties, ctx); !failed.empty()) {
        throw gr::exception(std::format("settings from property_map could not be loaded: {}", failed));
    }
}

// the member-type leaves the declarations at the end of Settings.hpp promise
// clang-format off
#define X(T)                                                                                                                \
    template std::optional<std::string> detail::setParameterImpl<T>(std::string_view, const pmt::Value&, property_map&);     \
    template bool detail::autoUpdateImpl<T>(std::string_view, const pmt::Value&, const std::set<std::string>&, property_map&); \
    template std::expected<T, std::string> settings::convertParameter<T>(std::string_view, const pmt::Value&);               \
    template std::expected<T, std::string> settings::extractStagedValue<T>(const pmt::Value&, std::string_view);

GR_SETTINGS_MEMBER_TYPES
#undef X
// clang-format on

} // namespace gr
