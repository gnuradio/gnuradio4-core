#include <boost/ut.hpp>

#include <algorithm>
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

constexpr std::size_t kWindow = 4UZ;

// needs a window of samples to make one item and takes one sample per call, so what it still owes the loop is
// known only where the call ends: it publishes that requirement as its input port minimum from processBulk().
// The window it cannot fill is the tail the end of the stream leaves, and the epilogue is where it says so.
struct WindowedRelay : gr::Block<WindowedRelay> {
    gr::PortIn<int>  in;
    gr::PortOut<int> out;

    GR_MAKE_REFLECTABLE(WindowedRelay, in, out);

    std::size_t _nShortCalls = 0UZ; // calls offered fewer samples than the minimum the previous call published
    std::size_t _tail        = 0UZ; // what the stream left that no window could be filled from
    bool        _epilogueRan = false;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        in.min_samples = kWindow;
        if (inSpan.size() < kWindow) {
            _nShortCalls++;
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
        return gr::work::Status::OK;
    }

    gr::work::Status processEpilogue(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        _epilogueRan = true;
        _tail        = inSpan.size();
        outSpan.publish(0UZ);
        return gr::work::Status::OK;
    }
};

struct CountingSink : gr::Block<CountingSink> {
    gr::PortIn<int> in;

    GR_MAKE_REFLECTABLE(CountingSink, in);

    std::size_t _nReceived = 0UZ;

    void processOne(int) { _nReceived++; }
};

constexpr std::size_t kShortBranch = 10UZ;
constexpr std::size_t kLongBranch  = 1000UZ;
constexpr std::size_t kMaxPerCall  = 8UZ;

// publishes _nItems items, at most _maxPerCall per call, and ends its stream in the call that publishes the last one
struct CountedSource : gr::Block<CountedSource> {
    gr::PortOut<int> out;

    GR_MAKE_REFLECTABLE(CountedSource, out);

    std::size_t _nItems     = 0UZ;
    std::size_t _maxPerCall = 0UZ;
    std::size_t _nPublished = 0UZ;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min({outSpan.size(), _maxPerCall, _nItems - _nPublished});
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = static_cast<int>(_nPublished + i);
        }
        outSpan.publish(n);
        _nPublished += n;
        return _nPublished == _nItems ? gr::work::Status::DONE : gr::work::Status::OK;
    }
};

// takes everything its two asynchronous inputs hold and counts it per input
struct AsyncPairSink : gr::Block<AsyncPairSink> {
    gr::PortIn<int, gr::Async> first;
    gr::PortIn<int, gr::Async> second;

    GR_MAKE_REFLECTABLE(AsyncPairSink, first, second);

    std::size_t _nFirst  = 0UZ;
    std::size_t _nSecond = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& firstSpan, gr::InputSpanLike auto& secondSpan) {
        const std::size_t nFirst  = firstSpan.size();
        const std::size_t nSecond = secondSpan.size();
        std::ignore               = firstSpan.consume(nFirst);
        std::ignore               = secondSpan.consume(nSecond);
        _nFirst += nFirst;
        _nSecond += nSecond;
        return nFirst + nSecond > 0UZ ? gr::work::Status::OK : gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
    }
};

// counts like AsyncPairSink and ends its own stream at the end of its first input
struct FirstEndSink : gr::Block<FirstEndSink> {
    gr::PortIn<int, gr::Async> first;
    gr::PortIn<int, gr::Async> second;

    GR_MAKE_REFLECTABLE(FirstEndSink, first, second);

    std::size_t _nFirst  = 0UZ;
    std::size_t _nSecond = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& firstSpan, gr::InputSpanLike auto& secondSpan) {
        if (firstSpan.size() == 0UZ && gr::samples_to_eos_tag(first) == 0UZ) {
            std::ignore = firstSpan.consume(0UZ);
            std::ignore = secondSpan.consume(0UZ);
            this->requestStop();
            return gr::work::Status::OK;
        }
        const std::size_t nFirst  = firstSpan.size();
        const std::size_t nSecond = secondSpan.size();
        std::ignore               = firstSpan.consume(nFirst);
        std::ignore               = secondSpan.consume(nSecond);
        _nFirst += nFirst;
        _nSecond += nSecond;
        return nFirst + nSecond > 0UZ ? gr::work::Status::OK : gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
    }
};

// takes everything a synchronous and an asynchronous input hold and counts it per input
struct MixedPairSink : gr::Block<MixedPairSink> {
    gr::PortIn<int>            data;
    gr::PortIn<int, gr::Async> aux;

    GR_MAKE_REFLECTABLE(MixedPairSink, data, aux);

    std::size_t _nData              = 0UZ;
    std::size_t _nAux               = 0UZ;
    std::size_t _nCallsAfterDataEnd = 0UZ; // calls that found the synchronous input at its end

    gr::work::Status processBulk(gr::InputSpanLike auto& dataSpan, gr::InputSpanLike auto& auxSpan) {
        if (dataSpan.size() == 0UZ && gr::samples_to_eos_tag(data) == 0UZ) {
            _nCallsAfterDataEnd++;
        }
        const std::size_t nData = dataSpan.size();
        const std::size_t nAux  = auxSpan.size();
        std::ignore             = dataSpan.consume(nData);
        std::ignore             = auxSpan.consume(nAux);
        _nData += nData;
        _nAux += nAux;
        return nData + nAux > 0UZ ? gr::work::Status::OK : gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
    }
};

constexpr std::size_t kStreamItems = 1000UZ; // one source's stream, fed to a block over a direct edge and a slow branch

// forwards one sample per call. A branch through it delivers each sample later than a direct edge from the same source.
struct OneSamplePerCall : gr::Block<OneSamplePerCall> {
    gr::PortIn<int>  in;
    gr::PortOut<int> out;

    GR_MAKE_REFLECTABLE(OneSamplePerCall, in, out);

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        outSpan[0UZ] = inSpan[0UZ];
        std::ignore  = inSpan.consume(1UZ);
        outSpan.publish(1UZ);
        return gr::work::Status::OK;
    }
};

// takes what its two synchronous inputs hold together and counts the pairs whose values differ
struct SyncPairSink : gr::Block<SyncPairSink> {
    gr::PortIn<int> first;
    gr::PortIn<int> second;

    GR_MAKE_REFLECTABLE(SyncPairSink, first, second);

    std::size_t _nReceived          = 0UZ;
    std::size_t _nMismatched        = 0UZ;
    std::size_t _nCallsFirstEndOnly = 0UZ; // calls that found the end of the first input in view and none on the second

    gr::work::Status processBulk(gr::InputSpanLike auto& firstSpan, gr::InputSpanLike auto& secondSpan) {
        if (gr::samples_to_eos_tag(first).has_value() && !gr::samples_to_eos_tag(second).has_value()) {
            _nCallsFirstEndOnly++;
        }
        const std::size_t n = firstSpan.size();
        for (std::size_t i = 0UZ; i < n; ++i) {
            if (firstSpan[i] != secondSpan[i]) {
                _nMismatched++;
            }
        }
        std::ignore = firstSpan.consume(n);
        std::ignore = secondSpan.consume(n);
        _nReceived += n;
        return gr::work::Status::OK;
    }
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

    "an input minimum published from processBulk governs the next call and ends the stream"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_drain::BurstSource>();
        auto&     relay  = flow.emplaceBlock<qa_drain::WindowedRelay>();
        auto&     sink   = flow.emplaceBlock<qa_drain::CountingSink>();
        expect(flow.connect<"out", "in">(source, relay).has_value());
        expect(flow.connect<"out", "in">(relay, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "the block was offered a span it cannot use and the graph never ended";

        expect(eq(relay._nShortCalls, 0UZ)) << "the scheduler kept offering less than the published minimum";
        expect(eq(sink._nReceived, qa_drain::kBurst - qa_drain::kWindow + 1UZ)) << "one item per sample the window could be filled from";
        expect(relay._epilogueRan) << "a block whose minimum can no longer be met was not driven to its epilogue";
        expect(eq(relay._tail, qa_drain::kWindow - 1UZ)) << "the tail is what no window could be filled from";
    };

    "an asynchronous input's end leaves the block running while another input is owed items"_test = [] {
        gr::Graph flow;
        auto&     shortSource = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&     longSource  = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&     sink        = flow.emplaceBlock<qa_drain::AsyncPairSink>();

        shortSource._nItems     = qa_drain::kShortBranch;
        shortSource._maxPerCall = qa_drain::kShortBranch;
        longSource._nItems      = qa_drain::kLongBranch;
        longSource._maxPerCall  = qa_drain::kMaxPerCall;
        expect(flow.connect<"out", "first">(shortSource, sink).has_value());
        expect(flow.connect<"out", "second">(longSource, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "the graph did not end";

        expect(eq(sink._nFirst, qa_drain::kShortBranch));
        expect(eq(sink._nSecond, qa_drain::kLongBranch)) << "the block ended at the end of its short branch";
        expect(eq(sink._nFirst + sink._nSecond, qa_drain::kShortBranch + qa_drain::kLongBranch));
        expect(sink.state() == STOPPED);
    };

    "an asynchronous input's end leaves the block running while its synchronous input is owed samples"_test = [] {
        gr::Graph flow;
        auto&     shortSource = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&     longSource  = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&     sink        = flow.emplaceBlock<qa_drain::MixedPairSink>();

        shortSource._nItems     = qa_drain::kShortBranch;
        shortSource._maxPerCall = qa_drain::kShortBranch;
        longSource._nItems      = qa_drain::kLongBranch;
        longSource._maxPerCall  = qa_drain::kMaxPerCall;
        expect(flow.connect<"out", "aux">(shortSource, sink).has_value());
        expect(flow.connect<"out", "data">(longSource, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "the graph did not end";

        expect(eq(sink._nAux, qa_drain::kShortBranch));
        expect(eq(sink._nData, qa_drain::kLongBranch)) << "the block ended at the end of its asynchronous input";
        expect(sink.state() == STOPPED);
    };

    "a block with two synchronous inputs fed by branches of different latency receives every sample"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&     relay  = flow.emplaceBlock<qa_drain::OneSamplePerCall>();
        auto&     sink   = flow.emplaceBlock<qa_drain::SyncPairSink>();

        source._nItems     = qa_drain::kStreamItems;
        source._maxPerCall = qa_drain::kMaxPerCall;
        expect(flow.connect<"out", "first">(source, sink).has_value());
        expect(flow.connect<"out", "in">(source, relay).has_value());
        expect(flow.connect<"out", "second">(relay, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "the graph did not end";

        expect(gt(sink._nCallsFirstEndOnly, 0UZ)) << "the direct edge's end never reached the block ahead of the slow branch's";
        expect(eq(sink._nReceived, qa_drain::kStreamItems)) << "the block ended at the end of its direct edge";
        expect(eq(sink._nMismatched, 0UZ)) << "the two inputs did not advance together";
        expect(sink.state() == STOPPED);
    };

    "a synchronous input's end leaves the block running while its asynchronous input is owed items"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&     relay  = flow.emplaceBlock<qa_drain::OneSamplePerCall>();
        auto&     sink   = flow.emplaceBlock<qa_drain::MixedPairSink>();

        source._nItems     = qa_drain::kStreamItems;
        source._maxPerCall = qa_drain::kMaxPerCall;
        expect(flow.connect<"out", "data">(source, sink).has_value());
        expect(flow.connect<"out", "in">(source, relay).has_value());
        expect(flow.connect<"out", "aux">(relay, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "the graph did not end";

        expect(eq(sink._nData, qa_drain::kStreamItems));
        expect(eq(sink._nAux, qa_drain::kStreamItems)) << "the block ended at the end of its synchronous input";
        expect(gt(sink._nCallsAfterDataEnd, 0UZ)) << "the block did not run with its synchronous input at its end";
        expect(sink.state() == STOPPED);
    };

    "a block that requests its own stop at the end of one input ends there"_test = [] {
        gr::Graph flow;
        auto&     shortSource = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&     longSource  = flow.emplaceBlock<qa_drain::CountedSource>();
        auto&     sink        = flow.emplaceBlock<qa_drain::FirstEndSink>();

        shortSource._nItems     = qa_drain::kShortBranch;
        shortSource._maxPerCall = qa_drain::kShortBranch;
        longSource._nItems      = qa_drain::kLongBranch;
        longSource._maxPerCall  = qa_drain::kMaxPerCall;
        expect(flow.connect<"out", "first">(shortSource, sink).has_value());
        expect(flow.connect<"out", "second">(longSource, sink).has_value());

        qa_drain::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(qa_drain::runWithin(scheduler, std::chrono::duration_cast<std::chrono::milliseconds>(qa_drain::kRunBound))) << "the graph did not end after the block stopped";

        expect(eq(sink._nFirst, qa_drain::kShortBranch));
        expect(lt(sink._nSecond, qa_drain::kLongBranch)) << "the block ran past the end of its first input";
        expect(sink.state() == STOPPED);
        expect(longSource.state() == STOPPED) << "the source left without a reader did not stop";
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
