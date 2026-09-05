#include <boost/ut.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <utility>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

namespace qa_drain {

constexpr std::size_t kBurst = 8UZ;

// publishes the whole burst and ends the stream in the same call, so every item and the end-of-stream
// marker reach the block downstream in a single delivery
struct BurstSource : gr::Block<BurstSource> {
    gr::PortOut<int> out;

    GR_MAKE_REFLECTABLE(BurstSource, out);

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (outSpan.size() < kBurst) {
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        for (std::size_t i = 0UZ; i < kBurst; ++i) {
            outSpan[i] = static_cast<int>(i);
        }
        outSpan.publish(kBurst);
        return gr::work::Status::DONE;
    }
};

// takes one item per call off an asynchronous input, the shape of a block that emits one record at a time:
// the items it was handed outlive the arrival of the end-of-stream marker behind them
struct OneAtATime : gr::Block<OneAtATime> {
    gr::PortIn<int, gr::Async>  in;
    gr::PortOut<int, gr::Async> out;

    GR_MAKE_REFLECTABLE(OneAtATime, in, out);

    std::size_t _nForwarded = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        if (inSpan.size() == 0UZ) {
            std::ignore = inSpan.consume(0UZ);
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        if (outSpan.size() == 0UZ) {
            std::ignore = inSpan.consume(0UZ);
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        outSpan[0UZ] = inSpan[0UZ];
        std::ignore  = inSpan.consume(1UZ);
        outSpan.publish(1UZ);
        _nForwarded++;
        return gr::work::Status::OK;
    }
};

// never takes what it was handed: the graph has to end anyway
struct StuckRelay : gr::Block<StuckRelay> {
    gr::PortIn<int, gr::Async>  in;
    gr::PortOut<int, gr::Async> out;

    GR_MAKE_REFLECTABLE(StuckRelay, in, out);

    std::size_t _nCalls = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        _nCalls++;
        std::ignore = inSpan.consume(0UZ);
        outSpan.publish(0UZ);
        return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
    }
};

constexpr std::size_t kItemsBeforeFailure = 2UZ;

// forwards a couple of items and then fails, leaving the rest of the burst in its input queue
struct FailingRelay : gr::Block<FailingRelay> {
    gr::PortIn<int, gr::Async>  in;
    gr::PortOut<int, gr::Async> out;

    GR_MAKE_REFLECTABLE(FailingRelay, in, out);

    std::size_t _nForwarded = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        if (_nForwarded >= kItemsBeforeFailure) {
            std::ignore = inSpan.consume(0UZ);
            outSpan.publish(0UZ);
            return gr::work::Status::ERROR;
        }
        if (inSpan.size() == 0UZ || outSpan.size() == 0UZ) {
            std::ignore = inSpan.consume(0UZ);
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        outSpan[0UZ] = inSpan[0UZ];
        std::ignore  = inSpan.consume(1UZ);
        outSpan.publish(1UZ);
        _nForwarded++;
        return gr::work::Status::OK;
    }
};

struct CountingSink : gr::Block<CountingSink> {
    gr::PortIn<int> in;

    GR_MAKE_REFLECTABLE(CountingSink, in);

    std::size_t _nReceived = 0UZ;

    void processOne(int) { _nReceived++; }
};

using SerialScheduler = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>;

constexpr auto kRunBound = std::chrono::seconds(5);

// runAndWait() on its own thread with a deadline, so a graph that fails to end fails the assertion
// instead of hanging ctest
template<typename TScheduler>
[[nodiscard]] bool runWithin(TScheduler& scheduler, std::chrono::milliseconds bound) {
    std::mutex              mutex;
    std::condition_variable finished;
    bool                    returned = false;

    std::thread runner([&scheduler, &mutex, &finished, &returned] {
        std::ignore = scheduler.runAndWait();
        {
            std::lock_guard lock(mutex);
            returned = true;
        }
        finished.notify_one();
    });

    bool inTime = false;
    {
        std::unique_lock lock(mutex);
        inTime = finished.wait_for(lock, bound, [&returned] { return returned; });
    }
    if (!inTime) {
        scheduler.requestStop(); // release the run loop so the process can still exit
    }
    runner.join();
    return inTime;
}

} // namespace qa_drain

const boost::ut::suite<"end-of-stream drain"> drainTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "a block emitting one item per call is given every item it holds"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_drain::BurstSource>();
        auto&     relay  = flow.emplaceBlock<qa_drain::OneAtATime>();
        auto&     sink   = flow.emplaceBlock<qa_drain::CountingSink>();
        expect(flow.connect<"out", "in">(source, relay).has_value());
        expect(flow.connect<"out", "in">(relay, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "the graph did not end";

        expect(eq(relay._nForwarded, qa_drain::kBurst)) << "the end of the stream cut the block short of the items already in its queue";
        expect(eq(sink._nReceived, qa_drain::kBurst)) << "items accepted upstream never reached the sink";
    };

    "a block that never takes its remainder does not hold the graph open"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_drain::BurstSource>();
        auto&     relay  = flow.emplaceBlock<qa_drain::StuckRelay>();
        auto&     sink   = flow.emplaceBlock<qa_drain::CountingSink>();
        expect(flow.connect<"out", "in">(source, relay).has_value());
        expect(flow.connect<"out", "in">(relay, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "a block making no progress held the graph open";

        expect(gt(relay._nCalls, 1UZ)) << "the block was not offered its remainder at all";
        expect(eq(sink._nReceived, 0UZ));
        expect(relay.state() == STOPPED);
    };

    "a graph ending on an error does not drain"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_drain::BurstSource>();
        auto&     relay  = flow.emplaceBlock<qa_drain::FailingRelay>();
        auto&     sink   = flow.emplaceBlock<qa_drain::CountingSink>();
        expect(flow.connect<"out", "in">(source, relay).has_value());
        expect(flow.connect<"out", "in">(relay, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "the failing graph did not end";

        expect(scheduler.state() == ERROR) << "a failing block drives the scheduler to ERROR";
        expect(eq(relay._nForwarded, qa_drain::kItemsBeforeFailure)) << "the failed block was kept running to empty its queue";
        expect(le(sink._nReceived, qa_drain::kItemsBeforeFailure));
    };
};

int main() { /* tests are statically registered */ }
