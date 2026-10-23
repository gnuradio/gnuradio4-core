#include <boost/ut.hpp>

#include <algorithm>
#include <cstddef>
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
};

int main() { /* tests are statically registered */ }
