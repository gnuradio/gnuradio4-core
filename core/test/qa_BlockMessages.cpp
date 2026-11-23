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

// An Echo Set is answered by the framework handler and by nothing else, so the reply count on
// msgOut says which handler ran.
template<typename TBlock>
void requestEcho(TBlock& block, gr::MsgPortOut& toBlock, gr::MsgPortIn& fromBlock) {
    using namespace boost::ut;
    expect(block.msgOut.connect(fromBlock).has_value());
    expect(toBlock.connect(block.msgIn).has_value());
    gr::sendMessage<gr::message::Command::Set>(toBlock, "", gr::block::property::kEcho, gr::property_map{});
}

} // namespace qa_block_messages

const boost::ut::suite<"block message plumbing"> blockMessageTests = [] {
    using namespace boost::ut;
    using namespace gr;

    "a block that defines no processMessages() is served by the inherited one"_test = [] {
        qa_block_messages::InheritedHandler block;
        MsgPortOut                          toBlock;
        MsgPortIn                           fromBlock;
        qa_block_messages::requestEcho(block, toBlock, fromBlock);

        block.processScheduledMessages();

        expect(eq(fromBlock.streamReader().available(), 1UZ)) << "the inherited handler must answer the echo request";
    };

    "a Heartbeat Get is answered with the time of the reply"_test = [] {
        qa_block_messages::InheritedHandler block;
        MsgPortOut                          toBlock;
        MsgPortIn                           fromBlock;
        expect(block.msgOut.connect(fromBlock).has_value());
        expect(toBlock.connect(block.msgIn).has_value());
        sendMessage<message::Command::Get>(toBlock, "", block::property::kHeartbeat, property_map{});

        block.processScheduledMessages();

        const std::vector<Message> replies = qa_block_messages::takeMessages(fromBlock);
        expect(fatal(eq(replies.size(), 1UZ))) << "the Heartbeat Get is answered once";
        expect(fatal(replies[0].data.has_value()));
        expect(replies[0].data->contains("heartbeat")) << "the reply carries no time";
    };

    "a Heartbeat subscriber is told the time at each message pass until it unsubscribes"_test = [] {
        qa_block_messages::InheritedHandler block;
        MsgPortOut                          toBlock;
        MsgPortIn                           fromBlock;
        qa_block_messages::connect(block, toBlock, fromBlock);

        block.processScheduledMessages();
        expect(qa_block_messages::takeMessages(fromBlock).empty()) << "a block with no subscriber sends a heartbeat";

        sendMessage<message::Command::Subscribe>(toBlock, "", block::property::kHeartbeat, property_map{}, "client");
        block.processScheduledMessages(); // the Subscribe is taken after this pass's heartbeat
        expect(qa_block_messages::takeMessages(fromBlock).empty()) << "the Subscribe got a reply";
        block.processScheduledMessages();
        const std::vector<Message> beats = qa_block_messages::takeMessages(fromBlock);
        expect(fatal(eq(beats.size(), 1UZ))) << "one heartbeat per message pass";
        expect(beats[0].clientRequestID == "client" && beats[0].data.has_value() && beats[0].data->contains("heartbeat"));

        sendMessage<message::Command::Unsubscribe>(toBlock, "", block::property::kHeartbeat, property_map{}, "client");
        block.processScheduledMessages();
        std::ignore = qa_block_messages::takeMessages(fromBlock); // the heartbeat sent ahead of the Unsubscribe
        block.processScheduledMessages();
        expect(qa_block_messages::takeMessages(fromBlock).empty()) << "an unsubscribed client is told the time";
    };

    "a block that defines its own processMessages() hides the inherited one"_test = [] {
        qa_block_messages::OwnHandler block;
        MsgPortOut                    toBlock;
        MsgPortIn                     fromBlock;
        qa_block_messages::requestEcho(block, toBlock, fromBlock);

        block.processScheduledMessages();

        expect(eq(block._nCalls, 1UZ)) << "the dispatch must select the block's own handler";
        expect(eq(fromBlock.streamReader().available(), 0UZ)) << "the hidden handler must not also have answered";
    };

    "a block reaches the inherited handler by naming it"_test = [] {
        qa_block_messages::ForwardingHandler block;
        MsgPortOut                           toBlock;
        MsgPortIn                            fromBlock;
        qa_block_messages::requestEcho(block, toBlock, fromBlock);

        block.processScheduledMessages();

        expect(eq(block._nCalls, 1UZ)) << "the dispatch must select the block's own handler";
        expect(eq(fromBlock.streamReader().available(), 1UZ)) << "the qualified call must reach the inherited handler";
    };

    "a block that is no scheduler refuses a Subscribe to its lifecycle state"_test = [] {
        qa_block_messages::InheritedHandler block;
        MsgPortOut                          toBlock;
        MsgPortIn                           fromBlock;
        qa_block_messages::connect(block, toBlock, fromBlock);
        sendMessage<message::Command::Subscribe>(toBlock, "", block::property::kLifeCycleState, property_map{}, "client");
        block.processScheduledMessages();
        const std::vector<Message> replies = qa_block_messages::takeMessages(fromBlock);
        expect(fatal(eq(replies.size(), 1UZ))) << "the Subscribe got no reply";
        expect(!replies.front().data.has_value() && replies.front().clientRequestID == "client") << "the reply is no error to the client";
    };
};
