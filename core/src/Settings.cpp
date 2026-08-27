#include <gnuradio-4.0/Settings.hpp>

#include <cstdio>
#include <utility>

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

void reportValidationFailure(std::string_view key, const pmt::Value& value) { std::fputs(std::format("Failed to validate field '{}' with value '{}'.\n", key, value).c_str(), stderr); }

void reportConversionFailure(std::string_view key, std::string_view error) { std::fputs(std::format("Failed to convert key '{}': {}\n", key, error).c_str(), stderr); }

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

CtxSettingsBase::CtxSettingsBase(void* block, const settings::BlockDescriptor& descriptor) noexcept : _block(block), _descriptor(&descriptor) { _autoForwardParameters.insert(gr::tag::kDefaultTags.begin(), gr::tag::kDefaultTags.end()); }

// --- Simple accessors ---

const std::set<std::string>& CtxSettingsBase::writableMembers() const { return _descriptor->writableMembers; }

bool CtxSettingsBase::changed() const noexcept { return gr::atomic_ref(_changed).load_acquire(); }
void CtxSettingsBase::setChanged(bool b) noexcept { gr::atomic_ref(_changed).store_release(b); }

void CtxSettingsBase::setInitBlockParameters(const property_map& parameters) { _initBlockParameters = parameters; }

const SettingsCtx& CtxSettingsBase::activeContext() const noexcept { return _activeCtx; }

const std::set<std::string>& CtxSettingsBase::autoForwardParameters() const noexcept { return _autoForwardParameters; }

void CtxSettingsBase::addAutoForwardParameters(std::set<std::string> parameterKeys) {
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
            _activeCtx = bestMatchSettingsCtx.value();
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
    vec.erase(exactIt);

    if (vec.empty()) {
        _storedParameters.erase(ctx.context);
    }

    if (_activeCtx.context == ctx.context) {
        std::ignore = activateContextImpl({}); // Activate default context
    }

    return true;
}

// --- assignFrom ---

void CtxSettingsBase::assignFrom(const CtxSettingsBase& other) {
    std::scoped_lock lock(_mutex, other._mutex);
    gr::atomic_ref(_changed).store_release(gr::atomic_ref(other._changed).load_acquire());
    _storedParameters      = other._storedParameters;
    _defaultParameters     = other._defaultParameters;
    _initBlockParameters   = other._initBlockParameters;
    _autoUpdateParameters  = other._autoUpdateParameters;
    _autoForwardParameters = other._autoForwardParameters;
    _matchPred             = other._matchPred;
    _activeCtx             = other._activeCtx;
    _stagedParameters      = other._stagedParameters;
    _activeParameters      = other._activeParameters;
}

void CtxSettingsBase::assignFrom(CtxSettingsBase&& other) noexcept {
    std::scoped_lock lock(_mutex, other._mutex);
    gr::atomic_ref(_changed).store_release(gr::atomic_ref(other._changed).load_acquire());
    _storedParameters      = std::move(other._storedParameters);
    _defaultParameters     = std::move(other._defaultParameters);
    _initBlockParameters   = std::move(other._initBlockParameters);
    _autoUpdateParameters  = std::move(other._autoUpdateParameters);
    _autoForwardParameters = std::move(other._autoForwardParameters);
    _matchPred             = std::exchange(other._matchPred, settings::nullMatchPred);
    _activeCtx             = std::exchange(other._activeCtx, {});
    _stagedParameters      = std::move(other._stagedParameters);
    _activeParameters      = std::move(other._activeParameters);
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
    if (tag.map.contains(gr::tag::CONTEXT.shortKey())) {
        const pmt::Value& ctxInfo = tag.map.at(gr::tag::CONTEXT.shortKey());
        auto              result  = ctxInfo.value_or(std::string_view{});
        if (result.data() != nullptr) {
            return {std::string(result)};
        }
    }
    return std::nullopt;
}

std::optional<std::uint64_t> CtxSettingsBase::triggeredTimeInTag(const Tag& tag) const {
    if (tag.map.contains(gr::tag::TRIGGER_TIME.shortKey())) {
        const pmt::Value& pmtTimeUtcNs = tag.map.at(gr::tag::TRIGGER_TIME.shortKey());
        auto              result       = pmt::convert_safely<std::uint64_t>(pmtTimeUtcNs);
        if (result) {
            return *result;
        }
    }
    return std::nullopt;
}

std::optional<SettingsCtx> CtxSettingsBase::createSettingsCtxFromTag(const Tag& tag) const {
    // If CONTEXT is not present then return std::nullopt
    // IF TRIGGER_TIME is not present then time = now()

    if (auto ctxValue = contextInTag(tag); ctxValue.has_value()) {
        SettingsCtx ctx{};
        ctx.context = ctxValue.value();

        // update trigger time if present
        if (auto triggerTime = triggeredTimeInTag(tag); triggerTime.has_value()) {
            ctx.time = triggerTime.value();
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

    if (const property_map failed = set(_initBlockParameters); !failed.empty()) {
        throw gr::exception(std::format("settings could not be applied: {}", failed));
    }

    if (const auto failed = activateContext(); failed == std::nullopt) {
        throw gr::exception("Settings for context could not be activated");
    }
}

property_map CtxSettingsBase::set(const property_map& parameters, SettingsCtx ctx) {
    std::lock_guard lg(_mutex);
    return setImpl(parameters, ctx);
}

property_map CtxSettingsBase::setImpl(const property_map& parameters, SettingsCtx ctx) {
    const settings::BlockHooks& hooks = _descriptor->hooks;

    property_map ret;
    if (hooks.reflectable) {
        if (ctx.time == 0ULL) {
            ctx.time = settings::convertTimePointToUint64Ns(std::chrono::system_clock::now());
        }
#ifdef __EMSCRIPTEN__
        resolveDuplicateTimestamp(ctx);
#endif
        // initialize with empty property_map when best match parameters not found
        property_map newParameters = getBestMatchStoredParameters(ctx).value_or(_defaultParameters);
        if (!_autoUpdateParameters.contains(ctx)) {
            _autoUpdateParameters[ctx] = getBestMatchAutoUpdateParameters(ctx).value_or(_descriptor->writableMembers);
        }
        auto& currentAutoUpdateParameters = _autoUpdateParameters[ctx];

        for (const auto& [key, value] : parameters) {
            if (value.is_monostate()) {
                continue;
            }

            auto it = _descriptor->writableByName.find(key);
            if (it != _descriptor->writableByName.end()) {
                if (auto error = it->second->setParameter(key, value, newParameters)) {
                    throw gr::exception(*error);
                }
                // Remove from auto-update set if present
                if (auto autoIt = currentAutoUpdateParameters.find(std::string(key)); autoIt != currentAutoUpdateParameters.end()) {
                    currentAutoUpdateParameters.erase(autoIt);
                }
            } else {
                ret.insert_or_assign(key, value);
            }
        }
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
        for (const auto& [key, value] : parameters) {
            auto it = _descriptor->writableByName.find(key);
            if (it != _descriptor->writableByName.end()) {
                if (auto error = it->second->setParameter(key, value, _stagedParameters)) {
                    throw gr::exception(*error);
                }
            } else {
                ret.insert_or_assign(key, value);
            }
        }
    }
    if (!_stagedParameters.empty()) {
        setChanged(true);
    }
    return ret;
}

void CtxSettingsBase::storeDefaults() { storeCurrentParameters(_defaultParameters); }

void CtxSettingsBase::resetDefaults() {
    std::lock_guard lg(_mutex);
    resetDefaultsImpl();
}

void CtxSettingsBase::resetDefaultsImpl() {
    // add default parameters to stored and apply the parameters
    auto ctx = SettingsCtx{settings::convertTimePointToUint64Ns(std::chrono::system_clock::now()), std::string()};
#ifdef __EMSCRIPTEN__
    resolveDuplicateTimestamp(ctx);
#endif
    addStoredParameters(_defaultParameters, ctx);
    std::ignore = activateContextImpl({});
    std::ignore = applyStagedParametersImpl();

    removeExpiredStoredParameters();

    if (_descriptor->hooks.reset != nullptr) {
        _descriptor->hooks.reset(_block);
    }
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

    // fuzzy-match auto-update parameters (exact lookup may fail due to timestamp mismatch)
    if (!_autoUpdateParameters.contains(ctx)) {
        _autoUpdateParameters[ctx] = getBestMatchAutoUpdateParameters(ctx).value_or(_descriptor->writableMembers);
    }
    auto& autoUpdateParams = _autoUpdateParameters[ctx];

    const auto& parameters = tag.map;
    bool        wasChanged = false;
    for (const auto& [key, value] : parameters) {
        auto it = _descriptor->writableByName.find(key);
        if (it != _descriptor->writableByName.end() && (activeCtxChanged || !isActiveValueImpl(key, value))) {
            if (it->second->autoUpdate(key, value, autoUpdateParams, _stagedParameters)) {
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

ApplyStagedParametersResult CtxSettingsBase::applyStagedParameters() {
    std::unique_lock lock(_mutex);
    return applyStagedParametersImpl(&lock);
}

ApplyStagedParametersResult CtxSettingsBase::applyStagedParametersImpl(std::unique_lock<std::mutex>* reentrantLock) {
    const settings::BlockHooks& hooks = _descriptor->hooks;

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
        const property_map batch = std::exchange(_stagedParameters, {});

        // check if reset of settings should be performed
        if (batch.contains(static_cast<std::pmr::string>(gr::tag::RESET_DEFAULTS))) {
            resetDefaultsImpl();
        }

        property_map staged;
        for (const auto& [key, stagedValue] : batch) {
            auto it = _descriptor->writableByName.find(key);
            if (it != _descriptor->writableByName.end()) {
                if (!it->second->applyStaged(_block, key, stagedValue, result.appliedParameters, staged, hasSettingsChangedCallback)) {
                    result.failedParameters.insert_or_assign(key, stagedValue); // rejected: must not reach downstream blocks
                    continue;
                }
                if (_autoForwardParameters.contains(std::string(key))) {
                    result.forwardParameters.insert_or_assign(key, stagedValue);
                }
            }
        }

        updateActiveParametersImpl();

        // invoke user-callback function if staged is not empty; oldSettings, staged and result are local
        // copies, so the callback may read or write settings() without _mutex being held across it
        if (!staged.empty()) {
            if (reentrantLock != nullptr) {
                reentrantLock->unlock();
            }
            hooks.settingsChanged(_block, oldSettings, staged, result.forwardParameters);
            if (reentrantLock != nullptr) {
                reentrantLock->lock();
            }
        }

        updateActiveParametersImpl();

        // the settings keep the input rate; only the forwarded value carries the block's output rate
        if (hooks.chunkRatio != nullptr && result.forwardParameters.contains(gr::tag::SAMPLE_RATE.shortKey())) {
            float ratio = 1.0f;
            if (hooks.chunkRatio(_block, ratio)) {
                const auto activeIt = _activeParameters.find(gr::tag::SAMPLE_RATE.shortKey());
                // a sample_rate that is not a float (e.g. loaded as double from a GRC file) must not be dereferenced blindly
                const float* activeSampleRate = activeIt != _activeParameters.end() ? activeIt->second.get_if<float>() : nullptr;
                if (activeSampleRate != nullptr) {
                    const float newSampleRate = ratio * (*activeSampleRate);
                    result.forwardParameters.insert_or_assign(gr::tag::SAMPLE_RATE.shortKey(), newSampleRate);
                }
            }
        }

        if (batch.contains(static_cast<std::pmr::string>(gr::tag::STORE_DEFAULTS))) {
            storeDefaults();
        }

        if (hooks.reset != nullptr && batch.contains(static_cast<std::pmr::string>(gr::tag::RESET_DEFAULTS))) {
            hooks.reset(_block);
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
        member->readParameter(_block, member->name, _activeParameters);
    }
}

void CtxSettingsBase::storeCurrentParameters(property_map& parameters) {
    if (!_descriptor->hooks.reflectable) {
        return;
    }
    for (const settings::MemberDescriptor* member : _descriptor->readableMembers) {
        member->readParameter(_block, member->name, parameters);
    }
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
