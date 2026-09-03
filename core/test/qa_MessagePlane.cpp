#include <boost/ut.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Message.hpp>
#include <gnuradio-4.0/Port.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

namespace qa_msg {

// emits exactly one notification per work() invocation so the flood rate tracks the scheduler loop
struct MessageFlooder : gr::Block<MessageFlooder> {
    gr::PortOut<float> out;

    gr::Annotated<gr::Size_t, "notifications to emit before requesting stop"> n_messages = 1U;

    GR_MAKE_REFLECTABLE(MessageFlooder, out, n_messages);

    gr::Size_t _nEmitted = 0U;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_nEmitted >= n_messages) {
            outSpan.publish(0UZ);
            this->requestStop();
            return gr::work::Status::DONE;
        }
        this->emitMessage("qa_MessagePlane::flood", {{"seq", _nEmitted}});
        _nEmitted++;
        outSpan.publish(std::min(outSpan.size(), 1UZ));
        return gr::work::Status::OK;
    }
};

struct NullSink : gr::Block<NullSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(NullSink, in);

    std::size_t _nReceived = 0UZ;

    void processOne(float) { _nReceived++; }
};

// no ports: exists only to call processScheduledMessages() directly, outside a graph
struct SilentBlock : gr::Block<SilentBlock> {
    GR_MAKE_REFLECTABLE(SilentBlock);
};

[[nodiscard]] gr::Graph makeFloodGraph(gr::Size_t nMessages) {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<MessageFlooder>({{"n_messages", nMessages}});
    auto&     sink   = flow.emplaceBlock<NullSink>();
    expect(flow.connect<"out", "in">(source, sink).has_value());
    return flow;
}

} // namespace qa_msg

const boost::ut::suite<"message plane back-pressure"> messagePlaneTests = [] {
    using namespace boost::ut;
    using namespace gr;

    "a full message port drops instead of blocking the emitter"_test = [] {
        MsgPortOut port;
        auto       portBuffers = port.buffer();
        auto       idleReader  = portBuffers.streamBuffer.new_reader(); // subscribed but never consuming

        const std::size_t ringSize = portBuffers.streamBuffer.size();
        const std::size_t nBefore  = message::droppedMessageCount().load(std::memory_order_relaxed);

        for (std::size_t i = 0UZ; i < 2UZ * ringSize; ++i) {
            sendMessage<message::Command::Notify>(port, "qa_MessagePlane", "overflow", property_map{});
        }

        expect(eq(idleReader.available(), ringSize)) << "the ring should be full, not partially filled";
        expect(ge(message::droppedMessageCount().load(std::memory_order_relaxed) - nBefore, ringSize)) << "overflowing emissions must be counted as drops";
    };

    "a scheduler without a message subscriber drains its child ring"_test = [] {
        const gr::Size_t  nMessages = 10000U; // > the 4096-slot shared _fromChildMessagePort
        const std::size_t nBefore   = message::droppedMessageCount().load(std::memory_order_relaxed);

        gr::scheduler::Simple sched;
        expect(sched.exchange(qa_msg::makeFloodGraph(nMessages)).has_value());
        expect(sched.runAndWait().has_value());

        expect(eq(message::droppedMessageCount().load(std::memory_order_relaxed), nBefore)) << "the scheduler must consume the child ring on the no-subscriber path";
    };

    "a subscriber that never consumes must not wedge the scheduler"_test = [] {
        const gr::Size_t nMessages = 10000U;

        gr::scheduler::Simple sched;
        expect(sched.exchange(qa_msg::makeFloodGraph(nMessages)).has_value());

        MsgPortIn stalledSubscriber;
        expect(sched.msgOut.connect(stalledSubscriber).has_value());

        const std::size_t nBefore = message::droppedMessageCount().load(std::memory_order_relaxed);
        expect(sched.runAndWait().has_value());

        expect(gt(stalledSubscriber.streamReader().available(), 0UZ)) << "the subscriber should have received what fitted";
        expect(gt(message::droppedMessageCount().load(std::memory_order_relaxed) - nBefore, 0UZ)) << "messages that did not fit must be counted as drops";
    };

    "processScheduledMessages() sends a kHeartbeat notify only where a subscriber is registered"_test = [] {
        qa_msg::SilentBlock block;
        MsgPortIn           reader;
        expect(block.msgOut.connect(reader).has_value());

        block.processScheduledMessages();
        expect(eq(reader.streamReader().available(), 0UZ)) << "no subscriber is registered yet, so nothing should have been sent";

        block.propertySubscriptions[std::string(block::property::kHeartbeat)].insert("test-client");
        block.processScheduledMessages();
        expect(eq(reader.streamReader().available(), 1UZ)) << "a registered subscriber must still receive exactly one heartbeat per poll";
    };
};

int main() { /* tests are statically registered */ }
