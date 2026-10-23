#include <boost/ut.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/Message.hpp>
#include <gnuradio-4.0/Port.hpp>

namespace qa_block_messages {

// The three blocks below carry no ports: each exists only to have processScheduledMessages() called
// on it directly, which is the dispatch under test, and needs no graph to do it.

// defines no processMessages(), so the dispatch reaches the one BlockBase compiles once
struct InheritedHandler : gr::Block<InheritedHandler> {
    GR_MAKE_REFLECTABLE(InheritedHandler);
};

// defines its own, which hides BlockBase's and answers nothing
struct OwnHandler : gr::Block<OwnHandler> {
    GR_MAKE_REFLECTABLE(OwnHandler);

    std::size_t _nCalls = 0UZ;

    void processMessages(const gr::MsgPortInBuiltin&, std::span<const gr::Message>) { _nCalls++; }
};

// defines its own and then names BlockBase's explicitly, which is what a scheduler does
struct ForwardingHandler : gr::Block<ForwardingHandler> {
    GR_MAKE_REFLECTABLE(ForwardingHandler);

    std::size_t _nCalls = 0UZ;

    void processMessages(const gr::MsgPortInBuiltin& port, std::span<const gr::Message> messages) {
        _nCalls++;
        gr::BlockBase::processMessages(port, messages);
    }
};

// carries one setting, which a SettingsCtx Set stores per context
struct SettingHandler : gr::Block<SettingHandler> {
    float gain = 1.f;

    GR_MAKE_REFLECTABLE(SettingHandler, gain);
};

// every message the block has sent since the last call
inline std::vector<gr::Message> takeMessages(gr::MsgPortIn& fromBlock) {
    gr::ReaderSpanLike auto  span = fromBlock.streamReader().get(fromBlock.streamReader().available());
    std::vector<gr::Message> taken(span.begin(), span.end());
    std::ignore = span.consume(span.size());
    return taken;
}

inline void connect(gr::BlockLike auto& block, gr::MsgPortOut& toBlock, gr::MsgPortIn& fromBlock) {
    using namespace boost::ut;
    expect(block.msgOut.connect(fromBlock).has_value());
    expect(toBlock.connect(block.msgIn).has_value());
}

inline void sendSettingsCtxSet(gr::MsgPortOut& toBlock, std::string_view context, std::uint64_t time, const gr::property_map& parameters) {
    gr::sendMessage<gr::message::Command::Set>(toBlock, "", gr::block::property::kSettingsCtx, //
        gr::property_map{{gr::tag::CONTEXT.shortKey(), std::string(context)}, {gr::tag::CONTEXT_TIME.shortKey(), time}, {"parameters", parameters}}, "qa");
}

// the error replies among `messages` whose text contains `text`
inline std::size_t countErrors(std::span<const gr::Message> messages, std::string_view text = {}) {
    return static_cast<std::size_t>(std::ranges::count_if(messages, [text](const gr::Message& message) { return !message.data.has_value() && message.data.error().message.contains(text); }));
}

// the text a refusal carries to name `cap`
inline std::string capText(std::size_t cap) { return std::format(" {} ", cap); }

// A heartbeat Get is answered by the framework handler and by nothing else, so the reply count on
// msgOut says which handler ran.
template<typename TBlock>
void requestHeartbeat(TBlock& block, gr::MsgPortOut& toBlock, gr::MsgPortIn& fromBlock) {
    using namespace boost::ut;
    expect(block.msgOut.connect(fromBlock).has_value());
    expect(toBlock.connect(block.msgIn).has_value());
    gr::sendMessage<gr::message::Command::Get>(toBlock, "", gr::block::property::kHeartbeat, gr::property_map{});
}

} // namespace qa_block_messages

const boost::ut::suite<"block message plumbing"> blockMessageTests = [] {
    using namespace boost::ut;
    using namespace gr;

    "a block that defines no processMessages() is served by the inherited one"_test = [] {
        qa_block_messages::InheritedHandler block;
        MsgPortOut                          toBlock;
        MsgPortIn                           fromBlock;
        qa_block_messages::requestHeartbeat(block, toBlock, fromBlock);

        block.processScheduledMessages();

        expect(eq(fromBlock.streamReader().available(), 1UZ)) << "the inherited handler must answer the heartbeat request";
    };

    "a block that defines its own processMessages() hides the inherited one"_test = [] {
        qa_block_messages::OwnHandler block;
        MsgPortOut                    toBlock;
        MsgPortIn                     fromBlock;
        qa_block_messages::requestHeartbeat(block, toBlock, fromBlock);

        block.processScheduledMessages();

        expect(eq(block._nCalls, 1UZ)) << "the dispatch must select the block's own handler";
        expect(eq(fromBlock.streamReader().available(), 0UZ)) << "the hidden handler must not also have answered";
    };

    "a block reaches the inherited handler by naming it"_test = [] {
        qa_block_messages::ForwardingHandler block;
        MsgPortOut                           toBlock;
        MsgPortIn                            fromBlock;
        qa_block_messages::requestHeartbeat(block, toBlock, fromBlock);

        block.processScheduledMessages();

        expect(eq(block._nCalls, 1UZ)) << "the dispatch must select the block's own handler";
        expect(eq(fromBlock.streamReader().available(), 1UZ)) << "the qualified call must reach the inherited handler";
    };

    "a property notifies at most kMaxSubscribersPerProperty clients and refuses a further one"_test = [] {
        using enum gr::message::Command;
        constexpr std::size_t               kCap     = message::kMaxSubscribersPerProperty;
        constexpr std::size_t               nRefused = 3UZ;
        qa_block_messages::InheritedHandler block;
        MsgPortOut                          toBlock;
        MsgPortIn                           fromBlock;
        qa_block_messages::connect(block, toBlock, fromBlock);
        const auto clientID = [](std::size_t i) { return std::format("client-{}", i); };

        for (std::size_t i = 0UZ; i < kCap + nRefused; ++i) {
            sendMessage<Subscribe>(toBlock, "", block::property::kHeartbeat, property_map{}, clientID(i));
        }
        block.processScheduledMessages();
        std::vector<Message> replies = qa_block_messages::takeMessages(fromBlock);
        expect(eq(replies.size(), nRefused)) << "one reply per refused Subscribe";
        expect(eq(qa_block_messages::countErrors(replies, qa_block_messages::capText(kCap)), replies.size())) << "each reply is an error that names the cap";
        std::vector<std::string> refusedIDs = replies | std::views::transform(&Message::clientRequestID) | std::ranges::to<std::vector>();
        std::vector<std::string> pastCapIDs = std::views::iota(kCap, kCap + nRefused) | std::views::transform(clientID) | std::ranges::to<std::vector>();
        std::ranges::sort(refusedIDs);
        std::ranges::sort(pastCapIDs);
        expect(refusedIDs == pastCapIDs) << "the refused ids are the ones past the cap";

        sendMessage<Subscribe>(toBlock, "", block::property::kHeartbeat, property_map{}, clientID(0UZ));
        sendMessage<Unsubscribe>(toBlock, "", block::property::kHeartbeat, property_map{}, clientID(1UZ));
        sendMessage<Subscribe>(toBlock, "", block::property::kHeartbeat, property_map{}, clientID(kCap));
        block.processScheduledMessages(); // the heartbeat goes out ahead of the three requests
        replies = qa_block_messages::takeMessages(fromBlock);
        expect(eq(replies.size(), kCap)) << "one heartbeat per accepted id, and no reply to a held id or to a freed slot";
        expect(eq(qa_block_messages::countErrors(replies), 0UZ));

        block.processScheduledMessages();
        replies = qa_block_messages::takeMessages(fromBlock);
        expect(eq(replies.size(), kCap));
        expect(std::ranges::any_of(replies, [&](const Message& reply) { return reply.clientRequestID == clientID(kCap); })) << "the freed slot went to the new id";
        expect(std::ranges::none_of(replies, [&](const Message& reply) { return reply.clientRequestID == clientID(1UZ); }));
    };

    "a block stores at most kMaxContexts contexts and kMaxPendingSets future sets per context"_test = [] {
        constexpr std::size_t             kMaxContexts    = CtxSettingsBase::kMaxContexts;
        constexpr std::size_t             kMaxPendingSets = CtxSettingsBase::kMaxPendingSets;
        constexpr std::size_t             nRefused        = 3UZ;
        const property_map                parameters{{"gain", 2.f}};
        const std::uint64_t               later = settings::convertTimePointToUint64Ns(std::chrono::system_clock::now() + std::chrono::hours(1));
        qa_block_messages::SettingHandler block;
        MsgPortOut                        toBlock;
        MsgPortIn                         fromBlock;
        qa_block_messages::connect(block, toBlock, fromBlock);

        const gr::Size_t nAutoUpdate = block.settings().getNAutoUpdateParameters();
        qa_block_messages::sendSettingsCtxSet(toBlock, "removed", later, parameters);
        sendMessage<message::Command::Disconnect>(toBlock, "", block::property::kSettingsCtx, property_map{{tag::CONTEXT.shortKey(), std::string("removed")}, {tag::CONTEXT_TIME.shortKey(), later}}, "qa");
        block.processScheduledMessages();
        expect(eq(qa_block_messages::countErrors(qa_block_messages::takeMessages(fromBlock)), 0UZ));
        expect(eq(block.settings().getNAutoUpdateParameters(), nAutoUpdate)) << "removing a context drops its auto-update entry";

        const std::size_t nStored = block.settings().getStoredAll().size();
        const std::size_t nNew    = kMaxContexts - nStored + nRefused;
        for (std::size_t i = 0UZ; i < nNew; ++i) {
            qa_block_messages::sendSettingsCtxSet(toBlock, std::format("ctx-{}", i), 0U, parameters);
        }
        qa_block_messages::sendSettingsCtxSet(toBlock, "ctx-0", 0U, parameters);
        block.processScheduledMessages();
        std::vector<Message> replies = qa_block_messages::takeMessages(fromBlock);
        expect(eq(replies.size(), nNew + 1UZ));
        expect(eq(qa_block_messages::countErrors(replies, qa_block_messages::capText(kMaxContexts)), nRefused)) << "a new context past the cap is refused, a stored one still takes a set";
        expect(eq(block.settings().getStoredAll().size(), kMaxContexts));

        for (std::size_t i = 0UZ; i < kMaxPendingSets + nRefused; ++i) {
            qa_block_messages::sendSettingsCtxSet(toBlock, "ctx-0", later + i, parameters);
        }
        qa_block_messages::sendSettingsCtxSet(toBlock, "ctx-0", 0U, parameters);
        block.processScheduledMessages();
        replies = qa_block_messages::takeMessages(fromBlock);
        expect(eq(qa_block_messages::countErrors(replies, qa_block_messages::capText(kMaxPendingSets)), nRefused)) << "a future set past the cap is refused, a set for now still takes";
        const auto stored = block.settings().getStoredAll();
        const auto ctx0   = stored.find(pmt::Value(std::string("ctx-0")));
        expect(ctx0 != stored.end());
        if (ctx0 != stored.end()) {
            expect(eq(static_cast<std::size_t>(std::ranges::count_if(ctx0->second, [&](const auto& pair) { return pair.context.time >= later; })), kMaxPendingSets));
        }
    };

    "a SettingsCtx Set returns a key no setting takes and leaves meta_information as it is"_test = [] {
        qa_block_messages::SettingHandler block;
        MsgPortOut                        toBlock;
        MsgPortIn                         fromBlock;
        qa_block_messages::connect(block, toBlock, fromBlock);
        const property_map metaInformation = block.meta_information.value;

        qa_block_messages::sendSettingsCtxSet(toBlock, "qa", 0U, property_map{{"gain", 2.f}, {"description", std::string("replaced")}, {"qa_note", std::string("filed")}});
        block.processScheduledMessages();
        const std::vector<Message> replies = qa_block_messages::takeMessages(fromBlock);
        expect(fatal(eq(replies.size(), 1UZ) && replies[0].data.has_value()));
        const auto  failedIt = replies[0].data->find("failed_to_set");
        const auto* failed   = failedIt != replies[0].data->end() ? failedIt->second.get_if<property_map>() : nullptr;
        expect(fatal(failed != nullptr));
        expect(failed->contains("description") && failed->contains("qa_note") && !failed->contains("gain")) << "the reply names the keys no setting takes";
        expect(block.meta_information.value == metaInformation) << "the unknown keys are not filed";
        expect(block.settings().getStored("gain", SettingsCtx{0U, std::string("qa")}) == std::optional<pmt::Value>(2.f)) << "the setting the block has still takes";
    };
};

int main() { /* tests are statically registered */ }
