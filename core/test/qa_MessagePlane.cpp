#include <boost/ut.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

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

constexpr std::string_view kDeviceLost = "the device disappeared";

// reports the loss of its device on its first call and then ends the stream, with ERROR or with DONE
struct DeviceLossSource : gr::Block<DeviceLossSource> {
    gr::PortOut<float> out;

    gr::Annotated<bool, "return ERROR after the report"> fail_after_report = true;

    GR_MAKE_REFLECTABLE(DeviceLossSource, out, fail_after_report);

    bool _reported = false;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (!_reported) {
            this->emitErrorMessage("processBulk", kDeviceLost);
            _reported = true;
            outSpan.publish(std::min(outSpan.size(), 1UZ));
            return gr::work::Status::OK;
        }
        outSpan.publish(0UZ);
        return fail_after_report ? gr::work::Status::ERROR : gr::work::Status::DONE;
    }
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

const boost::ut::suite<"a block error that ends the run"> blockErrorRunTests = [] {
    using namespace boost::ut;
    using namespace gr;

    "without a message subscriber the run's error names the block and carries its reason"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_msg::DeviceLossSource>();
        auto&     sink   = flow.emplaceBlock<qa_msg::NullSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        const std::string sourceName(source.unique_name);

        gr::scheduler::Simple sched;
        expect(sched.exchange(std::move(flow)).has_value());

        const std::expected<void, Error> result = sched.runAndWait();
        expect(!result.has_value()) << "a block error with no subscriber must fail the run";
        if (!result.has_value()) {
            expect(result.error().message.find(sourceName) != std::string::npos) << "the error must name the block that reported it: " << result.error().message;
            expect(result.error().message.find(qa_msg::kDeviceLost) != std::string::npos) << "the error must carry the block's reason: " << result.error().message;
        }
    };

    "with a message subscriber the run's error names the block and carries its reason"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_msg::DeviceLossSource>();
        auto&     sink   = flow.emplaceBlock<qa_msg::NullSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        const std::string sourceName(source.unique_name);

        gr::scheduler::Simple sched;
        MsgPortIn             subscriber;
        expect(sched.msgOut.connect(subscriber).has_value());
        expect(sched.exchange(std::move(flow)).has_value());

        const std::expected<void, Error> result = sched.runAndWait();
        expect(!result.has_value()) << "a block that ends its run with ERROR must fail the run";
        if (!result.has_value()) {
            expect(result.error().message.find(sourceName) != std::string::npos) << "the error must name the block that reported it: " << result.error().message;
            expect(result.error().message.find(qa_msg::kDeviceLost) != std::string::npos) << "the error must carry the block's reason: " << result.error().message;
        }
        expect(gt(subscriber.streamReader().available(), 0UZ)) << "the subscriber must still receive the block's error message";
    };

    "with a message subscriber a report the block survives leaves the run successful"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_msg::DeviceLossSource>({{"fail_after_report", false}});
        auto&     sink   = flow.emplaceBlock<qa_msg::NullSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        gr::scheduler::Simple sched;
        MsgPortIn             subscriber;
        expect(sched.msgOut.connect(subscriber).has_value());
        expect(sched.exchange(std::move(flow)).has_value());

        expect(sched.runAndWait().has_value()) << "a run that ends with DONE succeeds whatever its blocks reported";
        expect(source._reported) << "the block must have sent its report";
    };
};

int main() { /* tests are statically registered */ }
