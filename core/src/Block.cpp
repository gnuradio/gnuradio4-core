#include <gnuradio-4.0/Block.hpp>

namespace gr {

namespace {
// a Subscribe adds the client to the endpoint's subscribers and an Unsubscribe removes it; a new client past
// kMaxSubscribersPerProperty is refused
void subscribe(std::map<std::string, std::set<std::string>, std::less<>>& subscriptions, std::string_view endpoint, const Message& message) {
    std::set<std::string>& subscribers = subscriptions[std::string(endpoint)];
    if (message.cmd == message::Command::Unsubscribe) {
        subscribers.erase(message.clientRequestID);
    } else if (!subscribers.contains(message.clientRequestID) && subscribers.size() >= message::kMaxSubscribersPerProperty) {
        throw gr::exception(std::format("refused client {}: property {} already notifies {} clients, the cap kMaxSubscribersPerProperty", message.clientRequestID, endpoint, message::kMaxSubscribersPerProperty));
    } else {
        subscribers.insert(message.clientRequestID);
    }
}
} // namespace

void BlockBase::initStandardPropertyCallbacks() noexcept {
    propertyCallbacks = {
        {block::property::kHeartbeat, &BlockBase::propertyCallbackHeartbeat},
        {block::property::kEcho, &BlockBase::propertyCallbackEcho},
        {block::property::kLifeCycleState, &BlockBase::propertyCallbackLifecycleState},
        {block::property::kSetting, &BlockBase::propertyCallbackSettings},
        {block::property::kStagedSetting, &BlockBase::propertyCallbackStagedSettings},
        {block::property::kStoreDefaults, &BlockBase::propertyCallbackStoreDefaults},
        {block::property::kResetDefaults, &BlockBase::propertyCallbackResetDefaults},
        {block::property::kTagCouplings, &BlockBase::propertyCallbackTagCouplings},
        {block::property::kMetaInformation, &BlockBase::propertyCallbackMetaInformation},
        {block::property::kUiConstraints, &BlockBase::propertyCallbackUiConstraints},
    };
}

void BlockBase::emitMessage(std::string_view endpoint, property_map message, std::string_view clientRequestID) noexcept { //
    sendMessage<message::Command::Notify>(cbMsgOut(), cbUniqueName() /* serviceName */, endpoint, std::move(message), clientRequestID);
}

void BlockBase::notifyListeners(std::string_view endpoint, property_map message) noexcept {
    if (const auto it = propertySubscriptions.find(endpoint); it != propertySubscriptions.end()) {
        for (const std::string& clientID : it->second) {
            emitMessage(endpoint, message, clientID);
        }
    }
}

void BlockBase::emitErrorMessage(std::string_view endpoint, std::string_view errorMsg, std::string_view clientRequestID, std::source_location location) noexcept { //
    emitErrorMessageIfAny(endpoint, std::unexpected(Error(errorMsg, location)), clientRequestID);
}

void BlockBase::emitErrorMessage(std::string_view endpoint, Error e, std::string_view clientRequestID) noexcept { emitErrorMessageIfAny(endpoint, std::unexpected(e), clientRequestID); }

void BlockBase::emitErrorMessageIfAny(std::string_view endpoint, std::expected<void, Error> e, std::string_view clientRequestID) noexcept {
    if (!e.has_value()) [[unlikely]] {
        sendMessage<message::Command::Notify>(cbMsgOut(), cbUniqueName() /* serviceName */, endpoint, std::move(e.error()), clientRequestID);
    }
}

void BlockBase::processMessages([[maybe_unused]] const MsgPortInBuiltin& port, std::span<const Message> messages) {
    using enum gr::message::Command;
    assert(std::addressof(port) == std::addressof(cbMsgIn()) && "got a message on wrong port");

    for (const auto& message : messages) {
        if (!message.serviceName.empty() && message.serviceName != cbUniqueName() && message.serviceName != cbName()) {
            // Skip if target does not match the block's (unique) name and is not empty.
            continue;
        }

        auto it = propertyCallbacks.find(message.endpoint);
        if (it == propertyCallbacks.end()) {
            continue; // did not find matching property callback
        }
        BlockBase::PropertyCallback callback = it->second;

        std::optional<Message> retMessage;
        try {
            retMessage = (this->*callback)(message.endpoint, message); // N.B. life-time: message is copied
        } catch (const gr::exception& e) {
            retMessage       = Message{message};
            retMessage->data = std::unexpected(Error(e));
        } catch (const std::exception& e) {
            retMessage       = Message{message};
            retMessage->data = std::unexpected(Error(e));
        } catch (...) {
            retMessage       = Message{message};
            retMessage->data = std::unexpected(Error(std::format("unknown exception in Block {} property '{}'\n request message: {} ", cbUniqueName(), message.endpoint, message)));
        }

        if (!retMessage.has_value()) {
            continue; // function does not produce any return message
        }

        retMessage->cmd         = Final; // N.B. could enable/allow for partial if we return multiple messages (e.g. using coroutines?)
        retMessage->serviceName = cbUniqueName();
        sendReply(std::move(*retMessage));
    } // - end - for (const auto &message : messages) { ..
}

void BlockBase::sendReply(Message reply) {
    sendUnsentReplies();
    if (_unsentReplies.empty()) {
        WriterSpanLike auto msgSpan = cbMsgOut().streamWriter().tryReserve<SpanReleasePolicy::ProcessAll>(1UZ);
        if (!msgSpan.empty()) {
            msgSpan[0] = std::move(reply);
            msgSpan.publish(1UZ);
            return;
        }
    }
    _unsentReplies.push_back(std::move(reply));
    if (const std::size_t capacity = cbMsgOut().bufferSize(); _unsentReplies.size() > capacity) {
        const std::size_t nDropped = _unsentReplies.size() - capacity;
        _unsentReplies.erase(_unsentReplies.begin(), _unsentReplies.begin() + static_cast<std::ptrdiff_t>(nDropped));
        message::droppedMessageCount().fetch_add(nDropped, std::memory_order_relaxed);
    }
}

void BlockBase::sendUnsentReplies() {
    if (_unsentReplies.empty()) {
        return;
    }
    auto&             writer = cbMsgOut().streamWriter();
    const std::size_t nFit   = std::min(_unsentReplies.size(), writer.available());
    if (nFit == 0UZ) {
        return;
    }
    WriterSpanLike auto msgSpan = writer.tryReserve<SpanReleasePolicy::ProcessAll>(nFit);
    const auto          nSent   = static_cast<std::ptrdiff_t>(msgSpan.size());
    std::ranges::move(_unsentReplies.begin(), _unsentReplies.begin() + nSent, msgSpan.begin());
    msgSpan.publish(msgSpan.size());
    _unsentReplies.erase(_unsentReplies.begin(), _unsentReplies.begin() + nSent);
}

std::optional<Message> BlockBase::propertyCallbackHeartbeat(std::string_view propertyName, Message message) {
    using enum gr::message::Command;
    assert(propertyName == block::property::kHeartbeat);

    if (message.cmd == Set || message.cmd == Get) {
        const auto nanoseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        message.data           = pmt::Value::Map{{"heartbeat", nanoseconds}};
        return message;
    }
    if (message.cmd == Subscribe || message.cmd == Unsubscribe) {
        subscribe(propertySubscriptions, propertyName, message);
        return std::nullopt;
    }

    throw gr::exception(std::format("block {} property {} does not implement command {}, msg: {}", cbUniqueName(), propertyName, message.cmd, message));
}

std::optional<Message> BlockBase::propertyCallbackEcho(std::string_view propertyName, Message message) {
    using enum gr::message::Command;
    assert(propertyName == block::property::kEcho);

    if (message.cmd == Set) {
        return message; // mirror message as is
    }

    throw gr::exception(std::format("block {} property {} does not implement command {}, msg: {}", cbUniqueName(), propertyName, message.cmd, message));
}

std::optional<Message> BlockBase::propertyCallbackLifecycleState([[maybe_unused]] std::string_view propertyName, Message message) {
    using enum gr::message::Command;
    assert(propertyName == block::property::kLifeCycleState);

    if (message.cmd == Set) {
        if (!message.data.has_value() || !message.data.value().contains("state")) {
            throw gr::exception(std::format("propertyCallbackLifecycleState - cannot set block state w/o 'state' data msg: {}", message));
        }

        const auto& dataMap = message.data.value();
        auto        it      = dataMap.find("state");
        if (it == dataMap.end()) {
            throw gr::exception(std::format("propertyCallbackLifecycleState - state not found, msg: {}", message));
        }

        const auto stateStr = it->second.value_or(std::string_view{});
        if (!stateStr.data()) {
            throw gr::exception(std::format("propertyCallbackLifecycleState - state is not a string, msg: {}", message));
        }

        auto state = gr::meta::parseEnum<lifecycle::State>(stateStr);
        if (!state.has_value()) {
            throw gr::exception(std::format("propertyCallbackLifecycleState - invalid lifecycle::State conversion from {}, msg: {}", stateStr, message));
        }

        if (auto e = cbChangeStateTo(state.value()); !e) {
            throw gr::exception(std::format("propertyCallbackLifecycleState - error in state transition - what: {}", e.error().message, e.error().sourceLocation, e.error().errorTime));
        }

        return std::nullopt;
    }

    if (message.cmd == Get) {
        message.data = pmt::Value::Map{{"state", std::string(gr::meta::enumName(cbState()).value_or(""))}};
        return message;
    }

    throw gr::exception(std::format("propertyCallbackLifecycleState - does not implement command {}, msg: {}", message.cmd, message));
}

std::optional<Message> BlockBase::propertyCallbackSettings(std::string_view propertyName, Message message) {
    using enum gr::message::Command;
    using settings::detail::FrameworkAccess;
    assert(propertyName == block::property::kSetting);

    if (message.cmd == Set) {
        if (!message.data.has_value()) {
            throw gr::exception(std::format("block {} (aka. {}) cannot set {} w/o data msg: {}", cbUniqueName(), cbName(), propertyName, message));
        }
        // no reply carries a result, a request id or not; each refused key goes out as a notification
        if (const std::expected<settings::Values, settings::Rejected> staged = cbSettings().set(*message.data, settings::noWait); !staged) {
            FrameworkAccess::notifyRefused(cbSettings(), staged.error());
        }
        return std::nullopt;
    }
    if (message.cmd == Get) {
        message.data = settings::heldValues(cbSettings().get());
        return message;
    }
    if (message.cmd == Subscribe || message.cmd == Unsubscribe) {
        subscribe(propertySubscriptions, propertyName, message);
        return std::nullopt;
    }

    throw gr::exception(std::format("block {} property {} does not implement command {}, msg: {}", cbUniqueName(), propertyName, message.cmd, message));
}

std::optional<Message> BlockBase::propertyCallbackStagedSettings(std::string_view propertyName, Message message) {
    using enum gr::message::Command;
    using settings::detail::FrameworkAccess;
    assert(propertyName == block::property::kStagedSetting);

    if (message.cmd == Set) {
        if (!message.data.has_value()) {
            throw gr::exception(std::format("block {} (aka. {}) cannot set {} w/o data msg: {}", cbUniqueName(), cbName(), propertyName, message));
        }
        if (const std::expected<settings::Values, settings::Rejected> staged = cbSettings().set(*message.data, settings::noWait); !staged) {
            throw gr::exception(std::format("block {} refused {}: {}", cbUniqueName(), propertyName, settings::describeRejected(staged.error())));
        }
        if (message.clientRequestID.empty()) {
            return std::nullopt;
        }
        message.data = FrameworkAccess::pendingValues(cbSettings());
        return message;
    }
    if (message.cmd == Get) {
        message.data = FrameworkAccess::pendingValues(cbSettings());
        return message;
    }

    throw gr::exception(std::format("block {} property {} does not implement command {}, msg: {}", cbUniqueName(), propertyName, message.cmd, message));
}

std::optional<Message> BlockBase::propertyCallbackStoreDefaults(std::string_view propertyName, Message message) {
    using enum gr::message::Command;
    assert(propertyName == block::property::kStoreDefaults);

    if (message.cmd == Set) {
        settings::detail::FrameworkAccess::storeDefaults(cbSettings());
        return std::nullopt;
    }

    throw gr::exception(std::format("block {} property {} does not implement command {}, msg: {}", cbUniqueName(), propertyName, message.cmd, message));
}

std::optional<Message> BlockBase::propertyCallbackResetDefaults(std::string_view propertyName, Message message) {
    using enum gr::message::Command;
    assert(propertyName == block::property::kResetDefaults);

    if (message.cmd == Set) {
        if (const std::expected<settings::Values, settings::Rejected> reset = cbSettings().resetDefaults(settings::noWait); !reset) {
            settings::detail::FrameworkAccess::notifyRefused(cbSettings(), reset.error());
        }
        return std::nullopt;
    }

    throw gr::exception(std::format("block {} property {} does not implement command {}, msg: {}", cbUniqueName(), propertyName, message.cmd, message));
}

std::optional<Message> BlockBase::propertyCallbackTagCouplings(std::string_view propertyName, Message message) {
    using enum gr::message::Command;
    assert(propertyName == block::property::kTagCouplings);

    if (message.cmd == Set) {
        if (!message.data.has_value()) {
            throw gr::exception(std::format("block {} (aka. {}) cannot set {} w/o data msg: {}", cbUniqueName(), cbName(), propertyName, message));
        }
        if (const std::expected<void, settings::Rejected> coupled = cbSettings().setTagCouplings(*message.data); !coupled) {
            throw gr::exception(std::format("block {} refused {}: {}", cbUniqueName(), propertyName, settings::describeRejected(coupled.error())));
        }
        if (message.clientRequestID.empty()) {
            return std::nullopt;
        }
        message.data = cbSettings().tagCouplings();
        return message;
    }
    if (message.cmd == Get) {
        message.data = cbSettings().tagCouplings();
        return message;
    }

    throw gr::exception(std::format("block {} property {} does not implement command {}, msg: {}", cbUniqueName(), propertyName, message.cmd, message));
}

std::optional<Message> BlockBase::propertyCallbackMetaInformation(std::string_view propertyName, Message message) {
    using enum gr::message::Command;
    assert(propertyName == block::property::kMetaInformation);

    if (message.cmd == Get) {
        message.data = cbMetaInformation();
        return message;
    }

    throw gr::exception(std::format("block {} property {} does not implement command {}, msg: {}", cbUniqueName(), propertyName, message.cmd, message));
}

std::optional<Message> BlockBase::propertyCallbackUiConstraints(std::string_view propertyName, Message message) {
    using enum gr::message::Command;
    assert(propertyName == block::property::kUiConstraints);

    if (message.cmd == Set) {
        if (!message.data.has_value()) {
            throw gr::exception(std::format("block {} (aka. {}) cannot set {} w/o data msg: {}", cbUniqueName(), cbName(), propertyName, message));
        }
        if (const std::expected<settings::Values, settings::Rejected> staged = cbSettings().set(property_map{{"ui_constraints", *message.data}}, settings::noWait); !staged) {
            settings::detail::FrameworkAccess::notifyRefused(cbSettings(), staged.error());
        }
        return std::nullopt;
    }
    if (message.cmd == Get) {
        const std::expected<settings::Value, settings::Rejected> read = cbSettings().get("ui_constraints");
        if (!read) {
            throw gr::exception(std::format("block {} refused {}: {}", cbUniqueName(), propertyName, settings::describeRejected(read.error())));
        }
        message.data = read->as<property_map>().value_or(property_map{});
        return message;
    }

    throw gr::exception(std::format("block {} property {} does not implement command {}, msg: {}", cbUniqueName(), propertyName, message.cmd, message));
}

std::optional<Message> BlockBase::propertyCallbackSubgraphExport([[maybe_unused]] std::string_view propertyName, Message message) {
    if (_subgraphExportHandler != nullptr) {
        return _subgraphExportHandler(_subgraphExportContext, std::move(message));
    }
    return std::nullopt;
}

namespace detail {

Error primePortNotIdleError(std::size_t portIdx, std::size_t nSamples, std::source_location loc) { return Error(std::format("primePort({}, {}) - block must not be in RUNNING state", portIdx, nSamples), loc); }

Error primePortIndexError(std::size_t portIdx, std::size_t nSamples, std::size_t nPorts, std::source_location loc) { return Error(std::format("primePort({}, {}) failed: portIdx out of range [0, {}]", portIdx, nSamples, nPorts), loc); }

Error primePortUnexpectedError(std::size_t portIdx, std::size_t nSamples, std::source_location loc) { return Error(std::format("primePort({}, {}) - unexpected failure", portIdx, nSamples), loc); }

Error primePortUnconnectedError(std::size_t portIdx, std::size_t nSamples, std::string_view portName, std::source_location loc) { return Error(std::format("primePort({}, {}) - port {} ({}) is not connected", portIdx, nSamples, portIdx, portName), loc); }

Error primePortSampleCountError(std::size_t portIdx, std::size_t nRequested, std::size_t nPublished, std::source_location loc) { return Error(std::format("primePort({}, {}) - failed requested {} and got {} samples", portIdx, nRequested, nRequested, nPublished), loc); }

} // namespace detail

} // namespace gr
