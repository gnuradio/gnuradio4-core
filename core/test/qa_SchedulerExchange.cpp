#include <boost/ut.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Message.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/thread/thread_pool.hpp>

namespace qa_exchange {

std::atomic<std::size_t> gFirstGraphSamples{0UZ};
std::atomic<std::size_t> gSecondGraphSamples{0UZ};

// reacts to a message on its own msgIn, i.e. on the scheduler worker that also runs
// processScheduledMessages(): exchange() must not self-deadlock when called from that thread
struct SwapRequester : gr::Block<SwapRequester> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(SwapRequester, out);

    std::function<void()> _onSwapRequest;
    std::function<void()> _onReset;
    std::atomic<bool>*    _swapReturned = nullptr;

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }

    void reset() {
        if (_onReset) {
            _onReset();
        }
    }

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

// its stop() hook reports that it has begun and returns only once the test releases it, which holds the scheduler in
// REQUESTED_STOP. The hook clears the gate, and a later stop passes
struct HeldStopSource : gr::Block<HeldStopSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(HeldStopSource, out);

    std::atomic<bool>* _entered  = nullptr;
    std::atomic<bool>* _released = nullptr;

    void stop() {
        if (std::atomic<bool>* entered = std::exchange(_entered, nullptr); entered != nullptr) {
            entered->store(true);
            entered->notify_all();
            _released->wait(false);
        }
    }

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }
};

template<std::atomic<std::size_t>* counter>
struct CountingSink : gr::Block<CountingSink<counter>> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(CountingSink, in);

    void processOne(float) { counter->fetch_add(1UZ, std::memory_order_relaxed); }
};

using FirstSink         = CountingSink<&gFirstGraphSamples>;
using SecondSink        = CountingSink<&gSecondGraphSamples>;
using TestScheduler     = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>;
using SerialScheduler   = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>;
using BlockingScheduler = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreadedBlocking>;

// exposes the handler of a graph replacement by message, to call it from a thread of the test's choosing
struct GrcProbe : SerialScheduler {
    using SerialScheduler::propertyCallbackGraphGRC;
    using SerialScheduler::SerialScheduler;
};

// a single-threaded scheduler whose customExchange() hook reports that exchange() has passed its state checks and
// returns only once the test releases it. The test then decides what the scheduler does before the swap continues.
struct GatedScheduler : gr::scheduler::SchedulerBase<GatedScheduler, gr::scheduler::ExecutionPolicy::singleThreaded> {
    using gr::scheduler::SchedulerBase<GatedScheduler, gr::scheduler::ExecutionPolicy::singleThreaded>::SchedulerBase;

    std::atomic<bool>* _exchangeEntered  = nullptr;
    std::atomic<bool>* _exchangeReleased = nullptr;

    void customInit() {
        const gr::Graph flatGraph = gr::graph::flatten(*this->_graph);
        std::lock_guard lock(this->_executionOrderMutex);
        std::lock_guard guard(this->_adoptionBlocksMutex);
        this->_adoptionBlocks.assign(1UZ, {});
        this->_executionOrder->assign(1UZ, std::vector<std::shared_ptr<gr::BlockModel>>(flatGraph.blocks().begin(), flatGraph.blocks().end()));
    }

    void customExchange() {
        if (_exchangeEntered == nullptr) {
            return;
        }
        _exchangeEntered->store(true);
        _exchangeEntered->notify_all();
        _exchangeReleased->wait(false);
    }
};

// a scheduler whose hook for a satisfied transition reports that the request has read the state and returns only once
// the test releases it. The test sets the gate before one stop request that the state satisfies, and the thread that
// makes the request clears it
template<gr::scheduler::ExecutionPolicy execution>
struct SatisfiedStopGate : gr::scheduler::SchedulerBase<SatisfiedStopGate<execution>, execution> {
    using Base = gr::scheduler::SchedulerBase<SatisfiedStopGate<execution>, execution>;
    using Base::Base;

    std::atomic<std::atomic<bool>*> _satisfiedEntered{nullptr};
    std::atomic<bool>*              _satisfiedReleased = nullptr;

    void customInit() {
        const gr::Graph flatGraph = gr::graph::flatten(*this->_graph);
        std::lock_guard lock(this->_executionOrderMutex);
        std::lock_guard guard(this->_adoptionBlocksMutex);
        this->_adoptionBlocks.assign(1UZ, {});
        this->_executionOrder->assign(1UZ, std::vector<std::shared_ptr<gr::BlockModel>>(flatGraph.blocks().begin(), flatGraph.blocks().end()));
    }

    void transitionSatisfied(gr::lifecycle::State requested, gr::lifecycle::State satisfiedBy) {
        if (std::atomic<bool>* entered = _satisfiedEntered.exchange(nullptr); entered != nullptr) {
            entered->store(true);
            entered->notify_all();
            _satisfiedReleased->wait(false);
        }
        Base::transitionSatisfied(requested, satisfiedBy);
    }
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

// counts the samples of a graph that a replacement by message loads
struct ReplacementSink : gr::Block<ReplacementSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(ReplacementSink, in);

    void processOne(float) { gSecondGraphSamples.fetch_add(1UZ, std::memory_order_relaxed); }
};

// the graph that a replacement by message loads: an endless source and a sink that counts into gSecondGraphSamples
[[nodiscard]] std::string replacementGraphYaml() {
    using namespace boost::ut;
    static const bool registered = [] {
        std::ignore = gr::globalBlockRegistry().insert<SwapRequester>();
        std::ignore = gr::globalBlockRegistry().insert<ReplacementSink>();
        return true;
    }();
    std::ignore = registered;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<SwapRequester>();
    auto&     sink   = flow.emplaceBlock<ReplacementSink>();
    expect(flow.connect<"out", "in">(source, sink).has_value());
    return gr::saveGrc(gr::globalPluginLoader(), flow);
}

// a lifecycle command that sets the scheduler's state
[[nodiscard]] gr::Message makeStateChange(std::string_view schedulerName, gr::lifecycle::State state) {
    gr::Message message;
    message.cmd         = gr::message::Command::Set;
    message.serviceName = schedulerName;
    message.endpoint    = gr::block::property::kLifeCycleState;
    message.data        = gr::property_map{{"state", std::string(gr::meta::enumName(state).value_or(""))}};
    return message;
}

// sends a graph replacement, a reset and a start in one span to the scheduler's message port. One worker handles all
// three
void sendReplacementAndStart(gr::MsgPortOut& toScheduler, std::string_view schedulerName, std::string yaml) {
    using enum gr::lifecycle::State;
    auto span           = toScheduler.streamWriter().reserve<gr::SpanReleasePolicy::ProcessAll>(3UZ);
    span[0]             = makeGraphReplacement(std::move(yaml));
    span[0].serviceName = schedulerName;
    span[1]             = makeStateChange(schedulerName, INITIALISED);
    span[2]             = makeStateChange(schedulerName, RUNNING);
    span.publish(3UZ);
}

// sends a reset and a start in one span to the scheduler's message port. One worker handles both
void sendResetAndStart(gr::MsgPortOut& toScheduler, std::string_view schedulerName) {
    using enum gr::lifecycle::State;
    auto span = toScheduler.streamWriter().reserve<gr::SpanReleasePolicy::ProcessAll>(2UZ);
    span[0]   = makeStateChange(schedulerName, INITIALISED);
    span[1]   = makeStateChange(schedulerName, RUNNING);
    span.publish(2UZ);
}

constexpr std::string_view kTwoThreadPoolName = "qa_exchange_two_threads";

// a pool of two threads that the manager holds under kTwoThreadPoolName until the case ends. A scheduler on it gives a
// graph of two blocks two job lists on any number of hardware threads
struct TwoThreadPool {
    TwoThreadPool() { gr::thread_pool::Manager::instance().replacePool(std::string(kTwoThreadPoolName), std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kTwoThreadPoolName, gr::thread_pool::TaskType::CPU_BOUND, 2U, 2U), "CPU")); }
    TwoThreadPool(const TwoThreadPool&)            = delete;
    TwoThreadPool& operator=(const TwoThreadPool&) = delete;
    ~TwoThreadPool() { gr::thread_pool::Manager::instance().replacePool(std::string(kTwoThreadPoolName), gr::thread_pool::Manager::defaultCpuPool()); }
};

// a swap or a restart that a worker requested, as a scheduler finds it at its first reset: none, one that waits for its
// requester to claim it, or one that its requester has claimed and that waits for the other workers to leave
enum class PendingAtReset { unread, none, waiting, claimed };

// a multi-threaded scheduler that reads the pending swap or restart at its first reset. Its job lists are those of a
// multi-threaded Simple scheduler. The worker that takes the first lifecycle command from msgIn runs
// _beforeLifecycleCommands once, before it handles the command
struct PendingExchangeProbe : gr::scheduler::SchedulerBase<PendingExchangeProbe, gr::scheduler::ExecutionPolicy::multiThreaded> {
    using Base = gr::scheduler::SchedulerBase<PendingExchangeProbe, gr::scheduler::ExecutionPolicy::multiThreaded>;
    using Base::Base;

    std::atomic<PendingAtReset> _pendingAtFirstReset{PendingAtReset::unread};
    std::function<void()>       _beforeLifecycleCommands;

    void customInit() {
        const gr::Graph   flatGraph = gr::graph::flatten(*this->_graph);
        const std::size_t nBatches  = this->nJobLists(flatGraph.blocks().size());
        std::lock_guard   lock(this->_executionOrderMutex);
        std::lock_guard   guard(this->_adoptionBlocksMutex);
        this->_adoptionBlocks.assign(nBatches, {});
        *this->_executionOrder = gr::scheduler::detail::batchBlocks(flatGraph.blocks(), nBatches);
    }

    void customReset() {
        PendingAtReset unread = PendingAtReset::unread;
        _pendingAtFirstReset.compare_exchange_strong(unread, pendingExchange());
        customInit();
    }

    void processMessages(gr::MsgPortInBuiltin& port, std::span<const gr::Message> messages) {
        const bool lifecycleCommand = std::ranges::any_of(messages, [](const gr::Message& message) { return message.endpoint == gr::block::property::kLifeCycleState; });
        if (&port == &this->msgIn && lifecycleCommand) {
            if (std::function<void()> hold = std::exchange(_beforeLifecycleCommands, {}); hold) {
                hold();
            }
        }
        Base::processMessages(port, messages);
    }

    [[nodiscard]] PendingAtReset pendingExchange() {
        std::lock_guard guard(this->_runMutex);
        if (!this->_pendingExchange.has_value()) {
            return PendingAtReset::none;
        }
        return this->_pendingExchange->claimed ? PendingAtReset::claimed : PendingAtReset::waiting;
    }
};

[[nodiscard]] gr::Graph makeFirstGraph() {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<SwapRequester>();
    auto&     sink   = flow.emplaceBlock<FirstSink>();
    expect(flow.connect<"out", "in">(source, sink).has_value());
    return flow;
}

// emits for 200 ms after its first work() call and then returns DONE
struct TimedSource : gr::Block<TimedSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(TimedSource, out);

    std::chrono::steady_clock::time_point _endAt{};

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const auto now = std::chrono::steady_clock::now();
        if (_endAt == std::chrono::steady_clock::time_point{}) {
            _endAt = now + std::chrono::milliseconds(200);
        }
        if (now >= _endAt) {
            outSpan.publish(0UZ);
            return gr::work::Status::DONE;
        }
        outSpan.publish(std::min(outSpan.size(), 8UZ));
        return gr::work::Status::OK;
    }
};

// reads the flag that the destructor sets first
struct DestructionProbe : TestScheduler {
    using TestScheduler::TestScheduler;

    [[nodiscard]] bool destroying() { return gr::atomic_ref(this->_run.destroying).load_acquire(); }
};

[[nodiscard]] gr::Graph makeSecondGraph() {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<SwapRequester>();
    auto&     sink   = flow.emplaceBlock<SecondSink>();
    expect(flow.connect<"out", "in">(source, sink).has_value());
    return flow;
}

[[nodiscard]] gr::Graph makeTimedGraph() {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<TimedSource>();
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

// consumes every message waiting on the port and returns the lifecycle states among them, in order
[[nodiscard]] std::vector<std::string> takeStates(gr::MsgPortIn& port) {
    std::vector<std::string> states;
    auto                     messages = port.streamReader().get();
    for (const gr::Message& message : messages) {
        if (message.endpoint != gr::block::property::kLifeCycleState || !message.data.has_value()) {
            continue;
        }
        for (const auto& [key, value] : *message.data) {
            if (key == "state") {
                states.push_back(value.value_or(std::string()));
            }
        }
    }
    std::ignore = messages.consume(messages.size());
    return states;
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

    "a destruction that begins inside a deferred swap waits out no watchdog period"_test = [] {
        // timed on the wall clock. The swap passes its check of the destruction flag, and the old graph's reset() hook
        // then holds it until the destruction has begun and has passed its check of the state. The restart then reads
        // the flag as set, stops the scheduler and spawns no watchdog. The watchdog period is 2 s. The bound holds only
        // when the destruction retires the old run's watchdog at once and the restart runs nothing.
        constexpr auto kBound = std::chrono::milliseconds(1000);

        qa_exchange::gFirstGraphSamples  = 0UZ;
        qa_exchange::gSecondGraphSamples = 0UZ;

        std::atomic<bool>                            resetEntered{false};
        std::optional<qa_exchange::DestructionProbe> scheduler;
        scheduler.emplace(gr::property_map{{"watchdog_timeout", gr::Size_t(2'000)}});
        qa_exchange::DestructionProbe* probe     = &*scheduler;
        qa_exchange::SwapRequester*    requester = nullptr;

        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_exchange::SwapRequester>();
            auto&     sink   = flow.emplaceBlock<qa_exchange::FirstSink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());

            source._onSwapRequest = [probe] { std::ignore = probe->exchange(qa_exchange::makeTimedGraph()); };
            source._onReset       = [probe, &resetEntered] {
                if (resetEntered.exchange(true)) {
                    return;
                }
                for (std::size_t i = 0UZ; i < 3000UZ && !probe->destroying(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20)); // the destructor reads the state meanwhile
            };
            requester = &source;

            expect(scheduler->exchange(std::move(flow)).has_value());
        }

        expect(scheduler->changeStateTo(INITIALISED).has_value());
        expect(scheduler->changeStateTo(RUNNING).has_value());
        expect(qa_exchange::awaitCount(qa_exchange::gFirstGraphSamples, 1UZ)) << "first graph never ran";

        qa_exchange::requestSwap(*requester);
        expect(qa_exchange::awaitFlag(resetEntered)) << "the deferred swap never reset the old graph";

        const auto destructionStart = std::chrono::steady_clock::now();
        scheduler.reset();
        const auto destructionTime = std::chrono::steady_clock::now() - destructionStart;

        expect(destructionTime < kBound) << std::format("the destruction took {}", std::chrono::duration_cast<std::chrono::milliseconds>(destructionTime));
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

    "a swap whose state check precedes another thread's start is refused and the run is kept"_test = [] {
        qa_exchange::gFirstGraphSamples  = 0UZ;
        qa_exchange::gSecondGraphSamples = 0UZ;

        qa_exchange::GatedScheduler scheduler;
        expect(scheduler.exchange(qa_exchange::makeFiniteGraph<qa_exchange::FirstSink>()).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());

        std::atomic<bool> exchangeEntered{false};
        std::atomic<bool> exchangeReleased{false};
        scheduler._exchangeEntered  = &exchangeEntered;
        scheduler._exchangeReleased = &exchangeReleased;

        // the swap reads the initialized state and waits in the hook. Another thread then starts the run, and the swap
        // resumes only once the state reads RUNNING. The result holds the original graph while the run may still use it.
        std::optional<std::expected<gr::meta::indirect<gr::Graph>, gr::Error>> swapResult;

        std::thread swapper([&scheduler, &swapResult] { swapResult = scheduler.exchange(qa_exchange::makeFiniteGraph<qa_exchange::SecondSink>()); });
        expect(qa_exchange::awaitFlag(exchangeEntered)) << "exchange() never reached its hook";
        std::thread runner([&scheduler] { std::ignore = scheduler.changeStateTo(RUNNING); });
        expect(qa_exchange::awaitState(scheduler, RUNNING)) << "the run did not start while the swap waited";
        exchangeReleased.store(true);
        exchangeReleased.notify_all();
        swapper.join();

        expect(fatal(swapResult.has_value()));
        expect(!swapResult->has_value()) << "exchange() swapped the graph of a scheduler that started after its state check";
        if (!swapResult->has_value()) {
            expect(swapResult->error().message.contains("RUNNING")) << "the refusal does not name the state: " << swapResult->error().message;
        }
        expect(qa_exchange::awaitCount(qa_exchange::gFirstGraphSamples, qa_exchange::kFiniteSamples)) << "the original graph did not run to its end";
        expect(eq(qa_exchange::gSecondGraphSamples.load(std::memory_order_relaxed), 0UZ)) << "the refused graph ran";

        if (scheduler.state() == RUNNING) {
            expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        }
        runner.join();
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

    // The worker handles the replacement, which stops the run and keeps the new graph. The worker that handled the batch
    // claims it when it releases its count. The reset and the start follow in the same batch, and the start arrives while the swap is
    // pending. runAndWait() returns once the swap is applied, and every state the scheduler published is on the port
    "a graph replacement followed by a start in one message batch runs the new graph"_test = []<typename TPolicy>() {
        using TScheduler                 = typename TPolicy::type;
        qa_exchange::gFirstGraphSamples  = 0UZ;
        qa_exchange::gSecondGraphSamples = 0UZ;
        std::string yaml                 = qa_exchange::replacementGraphYaml();

        TScheduler     scheduler;
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(qa_exchange::makeFirstGraph()).has_value());
        gr::sendMessage<gr::message::Command::Subscribe>(toScheduler, scheduler.unique_name, gr::block::property::kLifeCycleState, gr::property_map{}, "states");

        std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });
        expect(qa_exchange::awaitCount(qa_exchange::gFirstGraphSamples, 1UZ)) << "first graph never ran";

        qa_exchange::sendReplacementAndStart(toScheduler, scheduler.unique_name, std::move(yaml));
        expect(qa_exchange::awaitCount(qa_exchange::gSecondGraphSamples, 1UZ)) << "the start after the replacement never ran the new graph";
        expect(scheduler.state() == RUNNING) << "the scheduler left RUNNING";
        std::ignore = qa_exchange::takeStates(fromScheduler);

        std::ignore = scheduler.changeStateTo(REQUESTED_STOP);
        runner.join();
        expect(qa_exchange::awaitState(scheduler, STOPPED)) << "the scheduler did not stop";
        const std::vector<std::string> statesAfterStop = qa_exchange::takeStates(fromScheduler);
        const std::string              resetState(gr::meta::enumName(INITIALISED).value_or(""));
        expect(std::ranges::find(statesAfterStop, resetState) == statesAfterStop.end()) << std::format("the scheduler was reset after the stop: {}", statesAfterStop);
    } | std::tuple<std::type_identity<qa_exchange::SerialScheduler>, std::type_identity<qa_exchange::BlockingScheduler>, std::type_identity<qa_exchange::TestScheduler>>{};

    // The source's worker holds in the source's message handler while the sink's worker handles the batch. It leaves
    // its loop once the replacement is recorded, and the reset in the batch waits for it. The replacement still waits
    // unclaimed at that reset. The worker that handles the batch claims it with the start, and the new graph runs.
    "a worker that leaves while another worker handles a replacement does not claim it"_test = [] {
        qa_exchange::gFirstGraphSamples  = 0UZ;
        qa_exchange::gSecondGraphSamples = 0UZ;
        std::string yaml                 = qa_exchange::replacementGraphYaml();

        qa_exchange::TwoThreadPool        pool;
        qa_exchange::PendingExchangeProbe scheduler({{"poolName", std::string(qa_exchange::kTwoThreadPoolName)}});
        gr::MsgPortOut                    toScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        std::atomic<bool>           holding{false};
        qa_exchange::SwapRequester* holder = nullptr;
        {
            gr::Graph flow;
            auto&     sink   = flow.emplaceBlock<qa_exchange::FirstSink>();
            auto&     source = flow.emplaceBlock<qa_exchange::SwapRequester>();
            expect(flow.connect<"out", "in">(source, sink).has_value());
            source._onSwapRequest = [&scheduler, &holding] {
                holding.store(true);
                for (std::size_t i = 0UZ; i < 3000UZ && scheduler.pendingExchange() == qa_exchange::PendingAtReset::none; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            };
            holder = &source; // the wrapper holding it lives on the heap, so this survives the move
            expect(scheduler.exchange(std::move(flow)).has_value());
        }

        std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });
        expect(qa_exchange::awaitCount(qa_exchange::gFirstGraphSamples, 1UZ)) << "first graph never ran";
        expect(eq(scheduler.jobs()->size(), 2UZ)) << "the source and the sink must run on two workers";

        qa_exchange::requestSwap(*holder);
        expect(qa_exchange::awaitFlag(holding)) << "the source's worker never held";
        qa_exchange::sendReplacementAndStart(toScheduler, scheduler.unique_name, std::move(yaml));
        expect(qa_exchange::awaitCount(qa_exchange::gSecondGraphSamples, 1UZ)) << "the start after the replacement never ran the new graph";
        expect(scheduler.state() == RUNNING) << "the scheduler left RUNNING";
        expect(scheduler._pendingAtFirstReset.load() == qa_exchange::PendingAtReset::waiting) << "the worker that left during the batch claimed the replacement";

        std::ignore = scheduler.changeStateTo(REQUESTED_STOP);
        runner.join();
        expect(qa_exchange::awaitState(scheduler, STOPPED)) << "the scheduler did not stop";
    };

    // The source's handler swaps the graph on its worker while the sink's worker holds before a reset and a start sent
    // in one span. The source's worker claims the swap when it leaves. The claimed swap still waits at the reset for
    // the sink's worker, the start joins it, and one thread applies both. The new graph runs.
    "a swap that a block's handler records is applied once with the start that another worker handles"_test = [] {
        qa_exchange::gFirstGraphSamples  = 0UZ;
        qa_exchange::gSecondGraphSamples = 0UZ;

        qa_exchange::TwoThreadPool        pool;
        qa_exchange::PendingExchangeProbe scheduler({{"poolName", std::string(qa_exchange::kTwoThreadPoolName)}});
        gr::MsgPortOut                    toScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        std::atomic<bool> handlerEntered{false};
        std::atomic<bool> commandsHeld{false};
        std::atomic<bool> swapRecorded{false};
        scheduler._beforeLifecycleCommands = [&commandsHeld, &swapRecorded] {
            commandsHeld.store(true);
            std::ignore = qa_exchange::awaitFlag(swapRecorded);
        };
        qa_exchange::SwapRequester* requester = nullptr;
        {
            gr::Graph flow;
            auto&     sink   = flow.emplaceBlock<qa_exchange::FirstSink>();
            auto&     source = flow.emplaceBlock<qa_exchange::SwapRequester>();
            expect(flow.connect<"out", "in">(source, sink).has_value());
            source._onSwapRequest = [&scheduler, &handlerEntered, &commandsHeld, &swapRecorded] {
                handlerEntered.store(true);
                std::ignore = qa_exchange::awaitFlag(commandsHeld);
                std::ignore = scheduler.exchange(qa_exchange::makeSecondGraph());
                swapRecorded.store(true);
            };
            requester = &source; // the wrapper holding it lives on the heap, so this survives the move
            expect(scheduler.exchange(std::move(flow)).has_value());
        }

        std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });
        expect(qa_exchange::awaitCount(qa_exchange::gFirstGraphSamples, 1UZ)) << "first graph never ran";
        expect(eq(scheduler.jobs()->size(), 2UZ)) << "the source and the sink must run on two workers";

        qa_exchange::requestSwap(*requester);
        expect(qa_exchange::awaitFlag(handlerEntered)) << "the source's worker never entered the handler";
        qa_exchange::sendResetAndStart(toScheduler, scheduler.unique_name);
        expect(qa_exchange::awaitFlag(swapRecorded)) << "the source's handler never swapped the graph";
        expect(qa_exchange::awaitCount(qa_exchange::gSecondGraphSamples, 1UZ)) << "the start never ran the swapped-in graph";
        expect(scheduler.state() == RUNNING) << "the scheduler left RUNNING";
        expect(scheduler._pendingAtFirstReset.load() == qa_exchange::PendingAtReset::claimed) << "the swap was taken for applying before the reset and the start on the other worker";

        std::ignore = scheduler.changeStateTo(REQUESTED_STOP);
        runner.join();
        expect(qa_exchange::awaitState(scheduler, STOPPED)) << "the scheduler did not stop";
    };

    // a second thread requests a stop while the scheduler reads STOPPED. The request is satisfied and held after its
    // state read, while the test starts the next run. The stop belongs to the run that had ended
    "a stop request satisfied by STOPPED leaves the next run running"_test = [] {
        qa_exchange::gFirstGraphSamples = 0UZ;

        qa_exchange::SatisfiedStopGate<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler;
        expect(scheduler.exchange(qa_exchange::makeFirstGraph()).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        std::thread firstRun([&scheduler] { std::ignore = scheduler.changeStateTo(RUNNING); });
        expect(qa_exchange::awaitCount(qa_exchange::gFirstGraphSamples, 1UZ)) << "first run never ran";
        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        firstRun.join();
        expect(qa_exchange::awaitState(scheduler, STOPPED)) << "the first run did not stop";

        std::atomic<bool> satisfiedEntered{false};
        std::atomic<bool> satisfiedReleased{false};
        scheduler._satisfiedReleased = &satisfiedReleased;
        scheduler._satisfiedEntered  = &satisfiedEntered;
        std::thread requester([&scheduler] { std::ignore = scheduler.changeStateTo(REQUESTED_STOP); });
        satisfiedEntered.wait(false);

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        const std::size_t samplesBefore = qa_exchange::gFirstGraphSamples.load();
        std::thread       secondRun([&scheduler] { std::ignore = scheduler.changeStateTo(RUNNING); });
        expect(qa_exchange::awaitCount(qa_exchange::gFirstGraphSamples, samplesBefore + 1UZ)) << "the second run never ran";

        satisfiedReleased.store(true);
        satisfiedReleased.notify_all();
        requester.join();
        const std::size_t samplesAfterRequest = qa_exchange::gFirstGraphSamples.load();
        expect(qa_exchange::awaitCount(qa_exchange::gFirstGraphSamples, samplesAfterRequest + 1UZ)) << "the earlier stop ended the second run";
        expect(scheduler.state() == RUNNING) << "the earlier stop moved the scheduler out of RUNNING";

        std::ignore = scheduler.changeStateTo(REQUESTED_STOP);
        secondRun.join();
        expect(qa_exchange::awaitState(scheduler, STOPPED)) << "the second run did not stop";
    };

    // A second thread requests a stop while exchange() stops the running graph for its swap. The request reads
    // REQUESTED_STOP and is held after its state read. Meanwhile exchange() resets the scheduler and restores the run on
    // the new graph. The stop came after the run began, and it ends the run that the swap restored
    "a stop request satisfied during a swap's own stop ends the run that the swap restores"_test = [] {
        qa_exchange::gFirstGraphSamples  = 0UZ;
        qa_exchange::gSecondGraphSamples = 0UZ;

        std::atomic<bool>                                                             stopEntered{false};
        std::atomic<bool>                                                             stopReleased{false};
        qa_exchange::SatisfiedStopGate<gr::scheduler::ExecutionPolicy::multiThreaded> scheduler;
        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_exchange::HeldStopSource>();
            auto&     sink   = flow.emplaceBlock<qa_exchange::FirstSink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());
            source._released = &stopReleased;
            source._entered  = &stopEntered;
            expect(scheduler.exchange(std::move(flow)).has_value());
        }
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_exchange::awaitCount(qa_exchange::gFirstGraphSamples, 1UZ)) << "first graph never ran";

        std::atomic<bool> swapped{false};
        std::thread       swapper([&scheduler, &swapped] { swapped = scheduler.exchange(qa_exchange::makeSecondGraph()).has_value(); });
        stopEntered.wait(false);

        std::atomic<bool> satisfiedEntered{false};
        std::atomic<bool> satisfiedReleased{false};
        scheduler._satisfiedReleased = &satisfiedReleased;
        scheduler._satisfiedEntered  = &satisfiedEntered;
        std::thread requester([&scheduler] { std::ignore = scheduler.changeStateTo(REQUESTED_STOP); });
        satisfiedEntered.wait(false);

        stopReleased.store(true);
        stopReleased.notify_all();
        swapper.join();
        expect(swapped.load()) << "the swap failed";
        expect(qa_exchange::awaitCount(qa_exchange::gSecondGraphSamples, 1UZ)) << "the swap did not restore the run";

        satisfiedReleased.store(true);
        satisfiedReleased.notify_all();
        requester.join();
        expect(qa_exchange::awaitState(scheduler, STOPPED)) << "the stop requested during the swap's own stop left the restored run running";

        std::ignore = scheduler.changeStateTo(REQUESTED_STOP);
        expect(qa_exchange::awaitState(scheduler, STOPPED)) << "the scheduler did not stop";
    };
};

int main() { /* tests are statically registered */ }
