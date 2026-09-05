#include <boost/ut.hpp>

#include <cstddef>
#include <span>

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
};

int main() { /* tests are statically registered */ }
