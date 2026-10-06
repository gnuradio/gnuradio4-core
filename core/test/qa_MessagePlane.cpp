#include <boost/ut.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

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

constexpr std::string_view kSensorFault = "the sensor stopped answering";

// returns ERROR once another block has sent its report, and first reports kSensorFault itself when report_first is set
struct ErrorAfterReportSource : gr::Block<ErrorAfterReportSource> {
    gr::PortOut<float> out;

    gr::Annotated<bool, "report a fault before the ERROR"> report_first = false;

    GR_MAKE_REFLECTABLE(ErrorAfterReportSource, out, report_first);

    const bool* _awaitedReport = nullptr;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        outSpan.publish(0UZ);
        if (_awaitedReport == nullptr || !*_awaitedReport) {
            return gr::work::Status::OK;
        }
        if (report_first) {
            this->emitErrorMessage("processBulk", kSensorFault);
        }
        return gr::work::Status::ERROR;
    }
};

// no ports: exists only to call processScheduledMessages() directly, outside a graph
struct SilentBlock : gr::Block<SilentBlock> {
    GR_MAKE_REFLECTABLE(SilentBlock);
};

// exposes the replies the scheduler keeps for a full msgOut
struct ReplyKeepingScheduler : gr::scheduler::Simple<> {
    using gr::scheduler::Simple<>::_unforwardedReplies;
};

// messages to the kSetting property with the given command, each carrying its index as the client request ID
[[nodiscard]] std::vector<gr::Message> makeNumberedMessages(std::size_t count, gr::message::Command cmd, std::string_view serviceName) {
    std::vector<gr::Message> messages(count);
    for (std::size_t i = 0UZ; i < count; ++i) {
        messages[i].cmd             = cmd;
        messages[i].serviceName     = serviceName;
        messages[i].endpoint        = gr::block::property::kSetting;
        messages[i].clientRequestID = std::to_string(i);
    }
    return messages;
}

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

    // one scheduler thread traverses the blocks in turn, and the failing source waits for the other block's report, so
    // the survivable report is the first error the scheduler receives
    "the run's error names the block whose ERROR ended the run, not an earlier report of another block"_test = [] {
        for (const bool reportFirst : {false, true}) {
            gr::Graph flow;
            auto&     reporter     = flow.emplaceBlock<qa_msg::DeviceLossSource>({{"fail_after_report", false}});
            auto&     reporterSink = flow.emplaceBlock<qa_msg::NullSink>();
            auto&     failing      = flow.emplaceBlock<qa_msg::ErrorAfterReportSource>({{"report_first", reportFirst}});
            auto&     failingSink  = flow.emplaceBlock<qa_msg::NullSink>();
            expect(flow.connect<"out", "in">(reporter, reporterSink).has_value());
            expect(flow.connect<"out", "in">(failing, failingSink).has_value());
            failing._awaitedReport = &reporter._reported;
            const std::string reporterName(reporter.unique_name);
            const std::string failingName(failing.unique_name);

            gr::scheduler::Simple sched;
            MsgPortIn             subscriber;
            expect(sched.msgOut.connect(subscriber).has_value());
            expect(sched.exchange(std::move(flow)).has_value());

            const std::expected<void, Error> result = sched.runAndWait();
            expect(reporter._reported) << "the surviving block must have sent its report";
            expect(!result.has_value()) << "a block that ends its run with ERROR must fail the run";
            if (!result.has_value()) {
                const std::string& message = result.error().message;
                expect(message.find(failingName) != std::string::npos) << "the error must name the block whose ERROR ended the run: " << message;
                expect(message.find(reporterName) == std::string::npos) << "the error must not name the block that survived its report: " << message;
                expect(message.find(qa_msg::kDeviceLost) == std::string::npos) << "the error must not carry the survived report: " << message;
                expect(eq(message.find(qa_msg::kSensorFault) != std::string::npos, reportFirst)) << "the error carries the failing block's own report exactly when it sent one: " << message;
            }
        }
    };
};

const boost::ut::suite<"replies kept for a full message output"> keptReplyTests = [] {
    using namespace boost::ut;
    using namespace gr;

    // The reader is connected and consumes nothing until msgOut is full and the replies past it have arrived.
    "a block keeps at most a full msgOut of replies and drops the oldest"_test = [] {
        qa_msg::SilentBlock block;
        MsgPortIn           reader;
        expect(block.msgOut.connect(reader).has_value());

        const std::size_t capacity = block.msgOut.bufferSize();
        for (std::size_t i = 0UZ; i < capacity; ++i) {
            block.emitMessage("fill", {});
        }
        expect(eq(reader.streamReader().available(), capacity)) << "the notifications should fill msgOut";

        constexpr std::size_t nOverflow = 3UZ;
        const auto            requests  = qa_msg::makeNumberedMessages(capacity + nOverflow, message::Command::Get, block.unique_name);
        const std::size_t     nBefore   = message::droppedMessageCount().load(std::memory_order_relaxed);
        block.processMessages(block.msgIn, requests);
        expect(eq(block._unsentReplies.size(), capacity)) << "the kept replies should stop at the size of msgOut";
        expect(eq(message::droppedMessageCount().load(std::memory_order_relaxed) - nBefore, nOverflow)) << "each reply past the bound should be counted as a drop";

        {
            auto notifications = reader.streamReader().get(); // the consume takes effect when the span is released
            expect(notifications.consume(notifications.size()));
        }
        block.processScheduledMessages();
        auto replies = reader.streamReader().get();
        expect(eq(replies.size(), capacity)) << "every kept reply should go out once msgOut has room";
        expect(eq(replies.front().clientRequestID, std::to_string(nOverflow))) << "the oldest replies should be the ones dropped";
        expect(eq(replies.back().clientRequestID, std::to_string(capacity + nOverflow - 1UZ))) << "the newest reply should be kept";
        expect(block._unsentReplies.empty());
    };

    "a scheduler keeps at most a full msgOut of its children's replies and drops the oldest"_test = [] {
        qa_msg::ReplyKeepingScheduler sched;
        MsgPortIn                     reader;
        expect(sched.msgOut.connect(reader).has_value());

        const std::size_t capacity = sched.msgOut.bufferSize();
        for (std::size_t i = 0UZ; i < capacity; ++i) {
            sendMessage<message::Command::Notify>(sched.msgOut, "qa_MessagePlane", "fill", property_map{});
        }
        expect(eq(reader.streamReader().available(), capacity)) << "the notifications should fill msgOut";

        constexpr std::size_t nOverflow = 3UZ;
        const std::size_t     nBefore   = message::droppedMessageCount().load(std::memory_order_relaxed);
        sched.forwardToMsgOut(qa_msg::makeNumberedMessages(capacity + nOverflow, message::Command::Final, "child"));
        expect(eq(sched._unforwardedReplies.size(), capacity)) << "the kept replies should stop at the size of msgOut";
        expect(eq(message::droppedMessageCount().load(std::memory_order_relaxed) - nBefore, nOverflow)) << "each reply past the bound should be counted as a drop";

        {
            auto notifications = reader.streamReader().get(); // the consume takes effect when the span is released
            expect(notifications.consume(notifications.size()));
        }
        sched.forwardToMsgOut({});
        auto replies = reader.streamReader().get();
        expect(eq(replies.size(), capacity)) << "every kept reply should go out once msgOut has room";
        expect(eq(replies.front().clientRequestID, std::to_string(nOverflow))) << "the oldest replies should be the ones dropped";
        expect(eq(replies.back().clientRequestID, std::to_string(capacity + nOverflow - 1UZ))) << "the newest reply should be kept";
        expect(sched._unforwardedReplies.empty());
    };
};

int main() { /* tests are statically registered */ }
