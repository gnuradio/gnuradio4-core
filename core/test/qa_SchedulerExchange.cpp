#include <boost/ut.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <thread>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Message.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

namespace qa_exchange {

std::atomic<std::size_t> gFirstGraphSamples{0UZ};
std::atomic<std::size_t> gSecondGraphSamples{0UZ};

// reacts to a message on its own msgIn, i.e. on the scheduler worker that also runs
// processScheduledMessages(): exchange() must not self-deadlock when called from that thread
struct SwapRequester : gr::Block<SwapRequester> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(SwapRequester, out);

    std::function<void()> _onSwapRequest;
    std::atomic<bool>*    _swapReturned = nullptr;

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }

    void processMessages(const gr::MsgPortInBuiltin&, std::span<const gr::Message> messages) {
        for (const gr::Message& message : messages) {
            if (message.endpoint != "swapGraph" || !_onSwapRequest) {
                continue;
            }
            _onSwapRequest();
            if (_swapReturned != nullptr) {
                _swapReturned->store(true);
                _swapReturned->notify_all();
            }
        }
    }
};

constexpr std::size_t kFiniteSamples = 4096UZ;

// publishes kFiniteSamples samples and then ends its stream
struct FiniteSource : gr::Block<FiniteSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(FiniteSource, out);

    std::size_t _nRemaining = kFiniteSamples;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const std::size_t nPublish = std::min(outSpan.size(), _nRemaining);
        for (std::size_t i = 0UZ; i < nPublish; ++i) {
            outSpan[i] = 1.0f;
        }
        outSpan.publish(nPublish);
        _nRemaining -= nPublish;
        return _nRemaining == 0UZ ? gr::work::Status::DONE : gr::work::Status::OK;
    }
};

// its start() hook reports that it has begun and returns only once the test releases it, which holds the scheduler's
// start() between the state change to RUNNING and the dispatch of its worker
struct HeldStartSource : gr::Block<HeldStartSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(HeldStartSource, out);

    std::atomic<bool>* _entered  = nullptr;
    std::atomic<bool>* _released = nullptr;

    void start() {
        _entered->store(true);
        _entered->notify_all();
        _released->wait(false);
    }

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }
};

template<std::atomic<std::size_t>* counter>
struct CountingSink : gr::Block<CountingSink<counter>> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(CountingSink, in);

    void processOne(float) { counter->fetch_add(1UZ, std::memory_order_relaxed); }
};

using FirstSink       = CountingSink<&gFirstGraphSamples>;
using SecondSink      = CountingSink<&gSecondGraphSamples>;
using TestScheduler   = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>;
using SerialScheduler = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>;

// exposes the handler of a graph replacement by message, to call it from a thread of the test's choosing
struct GrcProbe : SerialScheduler {
    using SerialScheduler::propertyCallbackGraphGRC;
    using SerialScheduler::SerialScheduler;
};

template<typename TSink>
[[nodiscard]] gr::Graph makeFiniteGraph() {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<FiniteSource>();
    auto&     sink   = flow.emplaceBlock<TSink>();
    expect(flow.connect<"out", "in">(source, sink).has_value());
    return flow;
}

[[nodiscard]] gr::Message makeGraphReplacement(std::string yaml) {
    gr::Message request;
    request.cmd      = gr::message::Command::Set;
    request.endpoint = gr::scheduler::property::kGraphGRC;
    request.data     = gr::property_map{{"value", std::move(yaml)}};
    return request;
}

[[nodiscard]] gr::Graph makeFirstGraph() {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<SwapRequester>();
    auto&     sink   = flow.emplaceBlock<FirstSink>();
    expect(flow.connect<"out", "in">(source, sink).has_value());
    return flow;
}

[[nodiscard]] gr::Graph makeSecondGraph() {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<SwapRequester>();
    auto&     sink   = flow.emplaceBlock<SecondSink>();
    expect(flow.connect<"out", "in">(source, sink).has_value());
    return flow;
}

[[nodiscard]] bool awaitFlag(const std::atomic<bool>& flag) {
    for (std::size_t i = 0UZ; i < 3000UZ && !flag.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return flag.load();
}

template<typename TScheduler>
[[nodiscard]] bool awaitState(const TScheduler& scheduler, gr::lifecycle::State expected) {
    for (std::size_t i = 0UZ; i < 3000UZ && scheduler.state() != expected; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return scheduler.state() == expected;
}

[[nodiscard]] bool awaitCount(const std::atomic<std::size_t>& counter, std::size_t atLeast) {
    for (std::size_t i = 0UZ; i < 3000UZ && counter.load(std::memory_order_relaxed) < atLeast; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return counter.load(std::memory_order_relaxed) >= atLeast;
}

void requestSwap(SwapRequester& block) {
    gr::Message request;
    request.cmd      = gr::message::Command::Set;
    request.endpoint = "swapGraph";
    request.data     = gr::property_map{};

    auto writer = block.msgIn.buffer().streamBuffer.new_writer();
    auto span   = writer.tryReserve<gr::SpanReleasePolicy::ProcessAll>(1UZ);
    boost::ut::expect(!span.empty()) << "could not queue the swap request";
    span[0] = std::move(request);
    span.publish(1UZ);
}

} // namespace qa_exchange

const boost::ut::suite<"scheduler graph exchange"> schedulerExchangeTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "a graph swap requested from a scheduler worker completes"_test = [] {
        qa_exchange::gFirstGraphSamples  = 0UZ;
        qa_exchange::gSecondGraphSamples = 0UZ;

        std::atomic<bool>           swapReturned{false};
        qa_exchange::TestScheduler  scheduler;
        qa_exchange::SwapRequester* requester = nullptr;

        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_exchange::SwapRequester>();
            auto&     sink   = flow.emplaceBlock<qa_exchange::FirstSink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());

            source._swapReturned  = &swapReturned;
            source._onSwapRequest = [&scheduler] { std::ignore = scheduler.exchange(qa_exchange::makeSecondGraph()); };
            requester             = &source; // the wrapper holding it lives on the heap, so this survives the move

            expect(scheduler.exchange(std::move(flow)).has_value());
        }

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_exchange::awaitCount(qa_exchange::gFirstGraphSamples, 1UZ)) << "first graph never ran";

        qa_exchange::requestSwap(*requester);

        expect(qa_exchange::awaitFlag(swapReturned)) << "exchange() never returned from the worker thread";
        expect(qa_exchange::awaitState(scheduler, RUNNING)) << "the deferred swap never restarted the scheduler";
        expect(qa_exchange::awaitCount(qa_exchange::gSecondGraphSamples, 1UZ)) << "the exchanged-in graph never processed samples";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_exchange::awaitState(scheduler, STOPPED)) << "scheduler did not settle after the swap";
    };

    "a restarted scheduler runs the graph it was given last"_test = [] {
        qa_exchange::gFirstGraphSamples  = 0UZ;
        qa_exchange::gSecondGraphSamples = 0UZ;

        qa_exchange::TestScheduler scheduler;

        gr::Graph first;
        auto&     firstSource = first.emplaceBlock<qa_exchange::SwapRequester>();
        auto&     firstSink   = first.emplaceBlock<qa_exchange::FirstSink>();
        expect(first.connect<"out", "in">(firstSource, firstSink).has_value());

        expect(scheduler.exchange(std::move(first)).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_exchange::awaitCount(qa_exchange::gFirstGraphSamples, 1UZ)) << "first graph never ran";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_exchange::awaitState(scheduler, STOPPED)) << "first graph did not stop";

        expect(scheduler.exchange(qa_exchange::makeSecondGraph()).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_exchange::awaitCount(qa_exchange::gSecondGraphSamples, 1UZ)) << "the second graph's blocks never ran";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_exchange::awaitState(scheduler, STOPPED)) << "second graph did not stop";
    };

    "a running single-threaded scheduler refuses a graph swap from another thread"_test = [] {
        qa_exchange::gFirstGraphSamples  = 0UZ;
        qa_exchange::gSecondGraphSamples = 0UZ;

        qa_exchange::SerialScheduler scheduler;
        expect(scheduler.exchange(qa_exchange::makeFirstGraph()).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());

        // a single-threaded run executes on the thread that requests RUNNING and returns when the run ends
        std::thread runner([&scheduler] { std::ignore = scheduler.changeStateTo(RUNNING); });
        expect(qa_exchange::awaitCount(qa_exchange::gFirstGraphSamples, 1UZ)) << "first graph never ran";

        std::atomic<bool> swapReturned{false};
        std::atomic<bool> swapRefused{false};
        std::thread       swapper([&scheduler, &swapReturned, &swapRefused] {
            swapRefused.store(!scheduler.exchange(qa_exchange::makeSecondGraph()).has_value());
            swapReturned.store(true);
        });

        expect(qa_exchange::awaitFlag(swapReturned)) << "exchange() blocked its caller";
        expect(swapRefused.load()) << "exchange() did not return an error";
        expect(scheduler.state() == RUNNING) << "the refused swap changed the scheduler's state";
        const std::size_t nFirstBefore = qa_exchange::gFirstGraphSamples.load(std::memory_order_relaxed);
        expect(qa_exchange::awaitCount(qa_exchange::gFirstGraphSamples, nFirstBefore + 1UZ)) << "the running graph stopped";
        expect(eq(qa_exchange::gSecondGraphSamples.load(std::memory_order_relaxed), 0UZ)) << "the refused graph ran";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        swapper.join();
        runner.join();
        expect(qa_exchange::awaitState(scheduler, STOPPED)) << "the scheduler did not stop";
    };

    "a single-threaded scheduler refuses a swap from another thread while its start is in progress"_test = [] {
        qa_exchange::gFirstGraphSamples  = 0UZ;
        qa_exchange::gSecondGraphSamples = 0UZ;

        std::atomic<bool> hookEntered{false};
        std::atomic<bool> hookReleased{false};
        gr::Graph         flow;
        auto&             source = flow.emplaceBlock<qa_exchange::HeldStartSource>();
        auto&             sink   = flow.emplaceBlock<qa_exchange::FirstSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        source._entered  = &hookEntered; // the wrapper holding the block lives on the heap. The pointers stay valid after the move.
        source._released = &hookReleased;

        qa_exchange::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());

        std::thread runner([&scheduler] { std::ignore = scheduler.changeStateTo(RUNNING); });
        expect(qa_exchange::awaitFlag(hookEntered)) << "the start hook never ran";
        expect(scheduler.state() == RUNNING) << "the scheduler did not read RUNNING during its start";

        std::atomic<bool> swapReturned{false};
        std::atomic<bool> swapRefused{false};
        std::thread       swapper([&scheduler, &swapReturned, &swapRefused] {
            swapRefused.store(!scheduler.exchange(qa_exchange::makeSecondGraph()).has_value());
            swapReturned.store(true);
        });
        const bool        returnedDuringStart = qa_exchange::awaitFlag(swapReturned);

        hookReleased.store(true);
        hookReleased.notify_all();
        expect(returnedDuringStart) << "exchange() waited for the start in progress";
        expect(swapRefused.load()) << "exchange() did not refuse the swap during the start";
        expect(qa_exchange::awaitCount(qa_exchange::gFirstGraphSamples, 1UZ)) << "the run did not begin after its start hook returned";
        expect(eq(qa_exchange::gSecondGraphSamples.load(std::memory_order_relaxed), 0UZ)) << "the refused graph ran";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        swapper.join();
        runner.join();
        expect(qa_exchange::awaitState(scheduler, STOPPED)) << "the scheduler did not stop";
    };

    "the thread that ran a finished single-threaded run swaps in and runs the next graph"_test = [] {
        qa_exchange::gFirstGraphSamples  = 0UZ;
        qa_exchange::gSecondGraphSamples = 0UZ;

        qa_exchange::SerialScheduler scheduler;
        expect(scheduler.exchange(qa_exchange::makeFiniteGraph<qa_exchange::FirstSink>()).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value()); // runs the finite graph on this thread and returns when it ends
        expect(eq(qa_exchange::gFirstGraphSamples.load(std::memory_order_relaxed), qa_exchange::kFiniteSamples)) << "the first graph did not run to its end";

        expect(scheduler.exchange(qa_exchange::makeFiniteGraph<qa_exchange::SecondSink>()).has_value()) << "the thread that ran the finished graph was refused";
        expect(eq(qa_exchange::gSecondGraphSamples.load(std::memory_order_relaxed), qa_exchange::kFiniteSamples)) << "the swapped-in graph did not run on this thread";

        if (scheduler.state() == RUNNING) {
            expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        }
        expect(qa_exchange::awaitState(scheduler, STOPPED)) << "the scheduler did not stop";
    };

    "a refused graph replacement by message keeps the running graph"_test = [] {
        qa_exchange::gFirstGraphSamples  = 0UZ;
        qa_exchange::gSecondGraphSamples = 0UZ;

        qa_exchange::GrcProbe scheduler;
        expect(scheduler.exchange(qa_exchange::makeFirstGraph()).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());

        std::thread runner([&scheduler] { std::ignore = scheduler.changeStateTo(RUNNING); });
        expect(qa_exchange::awaitCount(qa_exchange::gFirstGraphSamples, 1UZ)) << "first graph never ran";

        const std::size_t          nBlocks = scheduler.graph().blocks().size();
        const gr::Message          request = qa_exchange::makeGraphReplacement(gr::saveGrc(gr::globalPluginLoader(), gr::Graph{}));
        std::optional<gr::Message> refused = scheduler.propertyCallbackGraphGRC(gr::scheduler::property::kGraphGRC, request);
        expect(refused.has_value() && !refused->data.has_value()) << "the refused replacement sent no error reply";
        expect(eq(scheduler.graph().blocks().size(), nBlocks)) << "the refused replacement retired the running graph's blocks";
        const std::size_t nFirstBefore = qa_exchange::gFirstGraphSamples.load(std::memory_order_relaxed);
        expect(qa_exchange::awaitCount(qa_exchange::gFirstGraphSamples, nFirstBefore + 1UZ)) << "the running graph stopped";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        runner.join();
        expect(qa_exchange::awaitState(scheduler, STOPPED)) << "the scheduler did not stop";

        // the same request succeeds once the run has stopped: the refusal above came from the running worker alone
        std::optional<gr::Message> accepted = scheduler.propertyCallbackGraphGRC(gr::scheduler::property::kGraphGRC, request);
        expect(accepted.has_value() && accepted->data.has_value()) << "the stopped scheduler refused the replacement";
    };
};

int main() { /* tests are statically registered */ }
