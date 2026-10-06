#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <thread>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>

namespace qa_nested {

using Clock   = std::chrono::steady_clock;
using Millis  = std::chrono::duration<double, std::milli>;
using Parent  = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreadedBlocking>;
using Nested  = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>;
using Managed = gr::SchedulerWrapper<Nested>;

// The parent parks for kParkMs once a pass of its own blocks moves nothing. Its watchdog stays beyond every case.
constexpr gr::Size_t  kParkMs       = 500U;
constexpr gr::Size_t  kWatchdogMs   = 10'000U;
constexpr gr::Size_t  kNoInactivity = 0U;                            // a holder parks after its first pass that moves nothing
constexpr auto        kSettle       = std::chrono::milliseconds(20); // the parent reaches its park well inside this
constexpr auto        kEventBound   = std::chrono::seconds(5);
constexpr double      kWakeBoundMs  = static_cast<double>(kParkMs) / 10.0;
constexpr std::size_t kBurstLength  = 64UZ;

// state that a case shares with the blocks of its graphs across the scheduler threads
struct Probe {
    std::atomic<bool>        open{false};       // the gate of the gated blocks
    std::atomic<std::size_t> nWaiting{0UZ};     // calls of a closed gated block that found samples waiting
    std::atomic<std::size_t> nMoved{0UZ};       // samples that the timed block moved
    std::atomic<Clock::rep>  eventAt{0};        // when the timed block first moved samples with the gate open
    std::atomic<std::size_t> nSourceCalls{0UZ}; // calls of the idle source, one in each pass of its worker

    void reset() {
        open.store(false, std::memory_order_release);
        nWaiting.store(0UZ, std::memory_order_release);
        nMoved.store(0UZ, std::memory_order_release);
        eventAt.store(0, std::memory_order_release);
        nSourceCalls.store(0UZ, std::memory_order_release);
    }

    // a call that moves samples after the gate opened marks the event, once
    void recordMoved(std::size_t nSamples) {
        nMoved.fetch_add(nSamples, std::memory_order_acq_rel);
        if (open.load(std::memory_order_acquire)) {
            Clock::rep unset = 0;
            eventAt.compare_exchange_strong(unset, Clock::now().time_since_epoch().count(), std::memory_order_acq_rel);
        }
    }
};

// publishes kBurstLength samples after each start and nothing after them
struct BurstSource : gr::Block<BurstSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(BurstSource, out);

    std::size_t _nEmitted = 0UZ;

    void start() { _nEmitted = 0UZ; }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const std::size_t nPublish = std::min(outSpan.size(), kBurstLength - _nEmitted);
        _nEmitted += nPublish;
        outSpan.publish(nPublish);
        return nPublish == 0UZ ? gr::work::Status::INSUFFICIENT_INPUT_ITEMS : gr::work::Status::OK;
    }
};

// publishes as many samples as its output ring takes
struct FillingSource : gr::Block<FillingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(FillingSource, out);

    Probe* _probe = nullptr;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const std::size_t nPublish = outSpan.size();
        outSpan.publish(nPublish);
        if (nPublish == 0UZ) {
            return gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        _probe->recordMoved(nPublish);
        return gr::work::Status::OK;
    }
};

// publishes nothing and counts its calls, one in each pass of the worker that runs it
struct IdleSource : gr::Block<IdleSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(IdleSource, out);

    Probe* _probe = nullptr;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        _probe->nSourceCalls.fetch_add(1UZ, std::memory_order_relaxed);
        outSpan.publish(0UZ);
        return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
    }
};

// forwards its input while the probe's gate is open and holds it while the gate is closed
struct GatedForwarder : gr::Block<GatedForwarder> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(GatedForwarder, in, out);

    Probe* _probe = nullptr;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        const std::size_t nForward = _probe->open.load(std::memory_order_acquire) ? std::min(inSpan.size(), outSpan.size()) : 0UZ;
        if (nForward == 0UZ) {
            if (inSpan.size() > 0UZ) {
                _probe->nWaiting.fetch_add(1UZ, std::memory_order_acq_rel);
            }
            std::ignore = inSpan.consume(0UZ);
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        std::copy_n(inSpan.begin(), nForward, outSpan.begin());
        std::ignore = inSpan.consume(nForward);
        outSpan.publish(nForward);
        return gr::work::Status::OK;
    }
};

// takes its input while the probe's gate is open and holds it while the gate is closed
struct GatedSink : gr::Block<GatedSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(GatedSink, in);

    Probe* _probe = nullptr;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        if (!_probe->open.load(std::memory_order_acquire)) {
            if (inSpan.size() > 0UZ) {
                _probe->nWaiting.fetch_add(1UZ, std::memory_order_acq_rel);
            }
            std::ignore = inSpan.consume(0UZ);
            return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        std::ignore = inSpan.consume(inSpan.size());
        return gr::work::Status::OK;
    }
};

// records the time of its first sample after the probe's gate opened
struct TimedSink : gr::Block<TimedSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(TimedSink, in);

    Probe* _probe = nullptr;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        const std::size_t nTaken = inSpan.size();
        std::ignore              = inSpan.consume(nTaken);
        if (nTaken > 0UZ) {
            _probe->recordMoved(nTaken);
        }
        return gr::work::Status::OK;
    }
};

// takes every sample that reaches it
struct DrainSink : gr::Block<DrainSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(DrainSink, in);

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        std::ignore = inSpan.consume(inSpan.size());
        return gr::work::Status::OK;
    }
};

// a scheduler that supplies its own worker, the smallest loop that runs a job list
struct OwnWorkerScheduler : gr::scheduler::SchedulerBase<OwnWorkerScheduler, gr::scheduler::ExecutionPolicy::multiThreaded> {
    using Base = gr::scheduler::SchedulerBase<OwnWorkerScheduler, gr::scheduler::ExecutionPolicy::multiThreaded>;
    using Base::SchedulerBase;

    void customInit() {
        const gr::Graph flatGraph = gr::graph::flatten(*this->_graph);
        const auto      blocks    = flatGraph.blocks();

        std::lock_guard lock(this->_executionOrderMutex);
        std::lock_guard guard(this->_adoptionBlocksMutex);
        this->_adoptionBlocks.clear();
        this->_adoptionBlocks.resize(1UZ);
        this->_executionOrder->clear();
        this->_executionOrder->emplace_back(blocks.begin(), blocks.end());
    }

    void poolWorker(std::size_t runnerID, std::shared_ptr<gr::scheduler::JobLists> jobList, std::size_t generation) {
        std::shared_ptr<gr::Sequence> nRunningJobs = this->_nRunningJobs;
        gr::on_scope_exit             release      = [this, &nRunningJobs] { this->releaseWorkerCount(*nRunningJobs); };

        std::vector<std::shared_ptr<gr::BlockModel>> localBlockList;
        {
            std::lock_guard lock(this->_executionOrderMutex);
            localBlockList = jobList->at(runnerID);
        }

        while (gr::lifecycle::isActive(this->state()) && gr::atomic_ref(this->_run.generation).load_acquire() == generation) {
            const gr::work::Result result = this->traverseBlockListOnce(localBlockList);
            if (result.status == gr::work::Status::DONE || result.status == gr::work::Status::ERROR) {
                return;
            }
        }
    }
};

template<typename TPredicate>
[[nodiscard]] bool awaitCondition(TPredicate satisfied, std::chrono::milliseconds bound = kEventBound) {
    const auto deadline = Clock::now() + bound;
    while (!satisfied()) {
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

[[nodiscard]] Parent makeParent(gr::Size_t timeoutMs, gr::Size_t inactivityCount) { return Parent({{"timeout_ms", timeoutMs}, {"timeout_inactivity_count", inactivityCount}, {"watchdog_timeout", kWatchdogMs}}); }

// a managed subgraph of one block, with the block's ports exported under their own names
template<typename TBlock, typename TScheduler = Nested>
[[nodiscard]] std::shared_ptr<gr::SchedulerWrapper<TScheduler>> makeManaged(Probe& probe, bool exportsOutput) {
    using namespace boost::ut;

    gr::Graph interior;
    auto&     block = interior.emplaceBlock<TBlock>();
    block._probe    = std::addressof(probe);

    auto managed = std::make_shared<gr::SchedulerWrapper<TScheduler>>();
    managed->setGraph(std::move(interior));
    expect(managed->exportPort(true, std::string(block.unique_name), gr::PortDirection::INPUT, "in", "in").has_value());
    if (exportsOutput) {
        expect(managed->exportPort(true, std::string(block.unique_name), gr::PortDirection::OUTPUT, "out", "out").has_value());
    }
    return managed;
}

// an idle source feeding a sink, which counts the passes of the worker that runs the graph
void addPassCounter(gr::Graph& flow, Probe& probe) {
    using namespace boost::ut;

    auto& source  = flow.emplaceBlock<IdleSource>();
    auto& sink    = flow.emplaceBlock<DrainSink>();
    source._probe = std::addressof(probe);
    expect(flow.connect<"out", "in">(source, sink).has_value());
}

// a burst source, the given managed subgraph, which forwards the burst once its gate opens, a timed sink, and a pass
// counter
[[nodiscard]] gr::Graph makePublishingGraph(Probe& probe, const std::shared_ptr<gr::BlockModel>& subgraph) {
    using namespace boost::ut;

    gr::Graph flow;
    std::ignore                               = flow.emplaceBlock<BurstSource>();
    flow.emplaceBlock<TimedSink>()._probe     = std::addressof(probe);
    const std::shared_ptr<gr::BlockModel> sub = flow.addBlock(subgraph);
    expect(flow.connect(flow.blocks()[0], gr::PortDefinition{"out"}, sub, gr::PortDefinition{"in"}).has_value());
    expect(flow.connect(sub, gr::PortDefinition{"out"}, flow.blocks()[1], gr::PortDefinition{"in"}).has_value());
    addPassCounter(flow, probe);
    return flow;
}

// the condition that a gated block has found samples waiting
[[nodiscard]] auto inputWaiting(Probe& probe) {
    return [&probe] { return awaitCondition([&probe] { return probe.nWaiting.load(std::memory_order_acquire) > 0UZ; }); };
}

// runs the parent's worker on a thread of its own while `during` runs on the caller's, then stops the run
template<typename TDuring>
void runWhile(Parent& parent, TDuring during) {
    std::thread runner([&parent] { std::ignore = parent.runAndWait(); });
    during();
    parent.requestStop();
    runner.join();
}

// runs the parent, opens the probe's gate once `parked` holds and the parent has run no pass for kSettle, and returns
// the time from the opening to the timed event
template<typename TParked>
[[nodiscard]] std::optional<Millis> timeWakeAfterOpening(Parent& parent, Probe& probe, TParked parked) {
    using namespace boost::ut;

    probe.reset();
    std::optional<Millis> wake;
    runWhile(parent, [&] {
        const bool isParked = parked();
        expect(isParked) << "the graph did not reach the state the case opens the gate in";
        if (!isParked) {
            return;
        }
        std::this_thread::sleep_for(kSettle);
        const std::size_t nPassesBefore = probe.nSourceCalls.load(std::memory_order_acquire);
        std::this_thread::sleep_for(kSettle);
        expect(eq(probe.nSourceCalls.load(std::memory_order_acquire), nPassesBefore)) << "the parent ran passes before the gate opened";
        const Clock::time_point openedAt = Clock::now();
        probe.open.store(true, std::memory_order_release);
        if (awaitCondition([&probe] { return probe.eventAt.load(std::memory_order_acquire) != 0; })) {
            wake = Millis(Clock::time_point(Clock::duration(probe.eventAt.load(std::memory_order_acquire))) - openedAt);
        }
    });
    return wake;
}

// the parent's passes over a fixed window, read from a source the parent calls once in each pass
[[nodiscard]] std::size_t countIdlePasses(Parent& parent, Probe& probe, std::chrono::milliseconds window) {
    probe.reset();
    std::size_t nPasses = 0UZ;
    runWhile(parent, [&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the start's own passes are not idle cost
        const std::size_t before = probe.nSourceCalls.load(std::memory_order_acquire);
        std::this_thread::sleep_for(window);
        nPasses = probe.nSourceCalls.load(std::memory_order_acquire) - before;
    });
    return nPasses;
}

} // namespace qa_nested

const boost::ut::suite<"a managed subgraph's progress"> nestedProgressTests = [] {
    using namespace boost::ut;
    using namespace qa_nested;

    // A managed subgraph is a scheduler that a graph holds as a block. The scheduler holds the progress sequence of
    // the graph that holds it, and keeps it through its own exchange, its parent's exchange and the destruction of
    // the graph that handed it.
    "a managed subgraph holds the progress sequence of the graph that holds it"_test = [] {
        Probe     probe;
        auto      managed = makeManaged<GatedForwarder>(probe, true);
        gr::Graph flow    = makePublishingGraph(probe, gr::SchedulerModel::asBlockModelPtr(managed));
        expect(managed->blockRef().progress.get() == std::addressof(flow.progress())) << "the graph that adds a managed subgraph must hand it its progress sequence";

        Parent parent = makeParent(kParkMs, 0U);
        expect(parent.exchange(std::move(flow)).has_value());
        expect(managed->blockRef().progress.get() == std::addressof(parent.graph().progress())) << "a graph moved into a scheduler keeps the sequence it handed";

        gr::Graph replacement;
        replacement.emplaceBlock<GatedForwarder>()._probe = std::addressof(probe);
        auto interior                                     = managed->blockRef().exchange(std::move(replacement));
        expect(interior.has_value());
        expect(managed->blockRef().progress.get() == std::addressof(parent.graph().progress())) << "the subgraph's own exchange must keep the enclosing sequence";
        if (interior.has_value()) {
            std::ignore = managed->blockRef().exchange(std::move(*interior)); // the parent's edges name the exported ports of this interior
        }

        const gr::Sequence* retiredSequence = managed->blockRef().progress.get();
        {
            auto successor = makeManaged<GatedForwarder>(probe, true);
            auto retired   = parent.exchange(makePublishingGraph(probe, gr::SchedulerModel::asBlockModelPtr(successor)));
            expect(retired.has_value());
            expect(successor->blockRef().progress.get() == std::addressof(parent.graph().progress())) << "the subgraph of an exchanged-in graph must hold that graph's sequence";
            expect(managed->blockRef().progress.get() != std::addressof(parent.graph().progress())) << "the retired subgraph must not hold the new graph's sequence";
        } // the retired graph is destroyed here, and the managed subgraph outlives it
        expect(managed->blockRef().progress.get() == retiredSequence) << "the retired subgraph must keep its sequence";
        managed->blockRef().progress->incrementAndGet(); // the sequence outlived its graph; a sanitizer reports a freed one here
    };

    // The parent's own blocks are idle: the source has published its burst and the sink waits for the subgraph. The
    // subgraph's forwarder publishes into the ring the sink reads once the gate opens. The parent wakes on that
    // publish, run after run, and not at the end of its park.
    "a parked parent resumes when its managed subgraph publishes into a shared ring"_test = [] {
        Probe  probe;
        Parent parent = makeParent(kParkMs, 0U);
        expect(parent.exchange(makePublishingGraph(probe, gr::SchedulerModel::asBlockModelPtr(makeManaged<GatedForwarder>(probe, true)))).has_value());

        for (std::size_t run = 1UZ; run <= 2UZ; ++run) {
            const std::optional<Millis> wake = timeWakeAfterOpening(parent, probe, inputWaiting(probe));
            expect(wake.has_value()) << std::format("run {}: the sink never received the burst", run);
            if (wake.has_value()) {
                std::println("run {}: the parent's sink received the burst {:.3f} ms after the gate opened; the park lasts {} ms", run, wake->count(), kParkMs);
                expect(lt(wake->count(), kWakeBoundMs)) << std::format("run {}: the parent woke {:.1f} ms after the subgraph published", run, wake->count());
            }
            expect(eq(probe.nMoved.load(), kBurstLength)) << std::format("run {}: the sink must receive the whole burst", run);
        }
    };

    // The parent's source has filled the ring that the subgraph's sink reads. The sink frees the ring once the gate
    // opens. The parent wakes on that consume and its source publishes again.
    "a parked parent resumes when its managed subgraph frees a shared ring"_test = [] {
        Probe probe;

        gr::Graph flow;
        flow.emplaceBlock<FillingSource>()._probe = std::addressof(probe);
        const std::shared_ptr<gr::BlockModel> sub = flow.addBlock(gr::SchedulerModel::asBlockModelPtr(makeManaged<GatedSink>(probe, false)));
        expect(flow.connect(flow.blocks()[0], gr::PortDefinition{"out"}, sub, gr::PortDefinition{"in"}).has_value());
        addPassCounter(flow, probe);

        Parent parent = makeParent(kParkMs, 0U);
        expect(parent.exchange(std::move(flow)).has_value());

        const auto ringFull = [&probe] {
            return awaitCondition([&probe] {
                const std::size_t nPublished = probe.nMoved.load(std::memory_order_acquire);
                std::this_thread::sleep_for(kSettle);
                return nPublished > 0UZ && nPublished == probe.nMoved.load(std::memory_order_acquire) && probe.nWaiting.load(std::memory_order_acquire) > 0UZ;
            });
        };
        const std::optional<Millis> wake = timeWakeAfterOpening(parent, probe, ringFull);
        expect(wake.has_value()) << "the source never published again";
        if (wake.has_value()) {
            std::println("the parent's source published again {:.3f} ms after the gate opened; the park lasts {} ms", wake->count(), kParkMs);
            expect(lt(wake->count(), kWakeBoundMs)) << std::format("the parent woke {:.1f} ms after the subgraph freed the ring", wake->count());
        }
    };

    // The forwarder sits in a managed subgraph inside another managed subgraph. The middle scheduler exports the ports
    // of the inner one's block. The inner scheduler's publish reaches the outermost parent through the middle one.
    "a parked parent resumes when a subgraph nested two deep publishes into a shared ring"_test = [] {
        Probe probe;
        auto  inner = makeManaged<GatedForwarder>(probe, true);

        gr::Graph                             middleGraph;
        const std::shared_ptr<gr::BlockModel> innerBlock = middleGraph.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
        auto                                  middle     = std::make_shared<Managed>();
        middle->setGraph(std::move(middleGraph));
        expect(middle->exportPort(true, std::string(innerBlock->uniqueName()), gr::PortDirection::INPUT, "in", "in").has_value());
        expect(middle->exportPort(true, std::string(innerBlock->uniqueName()), gr::PortDirection::OUTPUT, "out", "out").has_value());

        Parent parent = makeParent(kParkMs, 0U);
        expect(parent.exchange(makePublishingGraph(probe, gr::SchedulerModel::asBlockModelPtr(middle))).has_value());
        expect(middle->blockRef().progress.get() == std::addressof(parent.graph().progress())) << "the middle subgraph must hold the parent's sequence";

        for (std::size_t run = 1UZ; run <= 2UZ; ++run) {
            const std::optional<Millis> wake = timeWakeAfterOpening(parent, probe, inputWaiting(probe));
            expect(wake.has_value()) << std::format("run {}: the sink never received the burst", run);
            if (wake.has_value()) {
                std::println("two deep, run {}: the parent's sink received the burst {:.3f} ms after the gate opened; the park lasts {} ms", run, wake->count(), kParkMs);
                expect(lt(wake->count(), kWakeBoundMs)) << std::format("run {}: the parent woke {:.1f} ms after the inner subgraph published", run, wake->count());
            }
            expect(eq(probe.nMoved.load(), kBurstLength)) << std::format("run {}: the sink must receive the whole burst", run);
        }
    };

    // The middle subgraph parks like the parent. It exports the inner subgraph's input to the parent and reads the
    // inner subgraph's output with a sink of its own. The inner scheduler's publish wakes the middle one.
    "a parked holder resumes when the subgraph it exports in part publishes into its ring"_test = [] {
        Probe probe;
        auto  inner = makeManaged<GatedForwarder>(probe, true);

        gr::Graph                             middleGraph;
        const std::shared_ptr<gr::BlockModel> innerBlock = middleGraph.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
        middleGraph.emplaceBlock<TimedSink>()._probe     = std::addressof(probe);
        expect(middleGraph.connect(innerBlock, gr::PortDefinition{"out"}, middleGraph.blocks()[1], gr::PortDefinition{"in"}).has_value());
        addPassCounter(middleGraph, probe);
        auto middle = std::make_shared<gr::SchedulerWrapper<Parent>>(gr::property_map{{"timeout_ms", kParkMs}, {"timeout_inactivity_count", kNoInactivity}, {"watchdog_timeout", kWatchdogMs}});
        middle->setGraph(std::move(middleGraph));
        expect(middle->exportPort(true, std::string(innerBlock->uniqueName()), gr::PortDirection::INPUT, "in", "in").has_value());

        gr::Graph flow;
        std::ignore                               = flow.emplaceBlock<BurstSource>();
        const std::shared_ptr<gr::BlockModel> sub = flow.addBlock(gr::SchedulerModel::asBlockModelPtr(middle));
        expect(flow.connect(flow.blocks()[0], gr::PortDefinition{"out"}, sub, gr::PortDefinition{"in"}).has_value());
        addPassCounter(flow, probe);

        Parent parent = makeParent(kParkMs, 0U);
        expect(parent.exchange(std::move(flow)).has_value());

        const std::optional<Millis> wake = timeWakeAfterOpening(parent, probe, inputWaiting(probe));
        expect(wake.has_value()) << "the middle subgraph's sink never received the burst";
        if (wake.has_value()) {
            std::println("exported in part: the middle subgraph's sink received the burst {:.3f} ms after the gate opened; the park lasts {} ms", wake->count(), kParkMs);
            expect(lt(wake->count(), kWakeBoundMs)) << std::format("the middle subgraph woke {:.1f} ms after the inner subgraph published", wake->count());
        }
        expect(eq(probe.nMoved.load(), kBurstLength)) << "the middle subgraph's sink must receive the whole burst";
    };

    // The managed subgraph is a scheduler that runs its job list in a worker of its own, which calls the base's
    // traversal and nothing else of the base's worker.
    "a parked parent resumes when a subgraph with its own worker publishes into a shared ring"_test = [] {
        Probe  probe;
        Parent parent = makeParent(kParkMs, 0U);
        expect(parent.exchange(makePublishingGraph(probe, gr::SchedulerModel::asBlockModelPtr(makeManaged<GatedForwarder, OwnWorkerScheduler>(probe, true)))).has_value());

        const std::optional<Millis> wake = timeWakeAfterOpening(parent, probe, inputWaiting(probe));
        expect(wake.has_value()) << "the sink never received the burst";
        if (wake.has_value()) {
            std::println("own worker: the parent's sink received the burst {:.3f} ms after the gate opened; the park lasts {} ms", wake->count(), kParkMs);
            expect(lt(wake->count(), kWakeBoundMs)) << std::format("the parent woke {:.1f} ms after the subgraph published", wake->count());
        }
        expect(eq(probe.nMoved.load(), kBurstLength)) << "the sink must receive the whole burst";
    };

    // A subgraph wakes its parent only for samples that cross a ring the two share. The three arms run the same parent
    // settings over the same window: the idle source feeding a sink; the same with an idle managed subgraph between
    // the two; the same with a managed subgraph beside them that moves samples among its own blocks without pause.
    "a parked parent wakes for no subgraph work it cannot use"_test = [] {
        constexpr gr::Size_t kIdleParkMs = 20U;
        constexpr auto       kWindow     = std::chrono::milliseconds(500);
        Probe                probe;
        Probe                busyProbe;
        busyProbe.open.store(true, std::memory_order_release);

        gr::Graph alone;
        addPassCounter(alone, probe);
        Parent parentAlone = makeParent(kIdleParkMs, 5U);
        expect(parentAlone.exchange(std::move(alone)).has_value());

        gr::Graph withIdle;
        withIdle.emplaceBlock<IdleSource>()._probe = std::addressof(probe);
        std::ignore                                = withIdle.emplaceBlock<DrainSink>();
        const std::shared_ptr<gr::BlockModel> idle = withIdle.addBlock(gr::SchedulerModel::asBlockModelPtr(makeManaged<GatedForwarder>(probe, true)));
        expect(withIdle.connect(withIdle.blocks()[0], gr::PortDefinition{"out"}, idle, gr::PortDefinition{"in"}).has_value());
        expect(withIdle.connect(idle, gr::PortDefinition{"out"}, withIdle.blocks()[1], gr::PortDefinition{"in"}).has_value());
        Parent parentWithIdle = makeParent(kIdleParkMs, 5U);
        expect(parentWithIdle.exchange(std::move(withIdle)).has_value());

        gr::Graph interior;
        auto&     busySource = interior.emplaceBlock<FillingSource>();
        auto&     busySink   = interior.emplaceBlock<DrainSink>();
        busySource._probe    = std::addressof(busyProbe);
        expect(interior.connect<"out", "in">(busySource, busySink).has_value());
        auto busy = std::make_shared<Managed>();
        busy->setGraph(std::move(interior));

        gr::Graph withBusy;
        addPassCounter(withBusy, probe);
        std::ignore           = withBusy.addBlock(gr::SchedulerModel::asBlockModelPtr(busy));
        Parent parentWithBusy = makeParent(kIdleParkMs, 5U);
        expect(parentWithBusy.exchange(std::move(withBusy)).has_value());

        const std::size_t nAlone      = countIdlePasses(parentAlone, probe, kWindow);
        const std::size_t nWithIdle   = countIdlePasses(parentWithIdle, probe, kWindow);
        const std::size_t nBusyBefore = busyProbe.nMoved.load(std::memory_order_acquire);
        const std::size_t nWithBusy   = countIdlePasses(parentWithBusy, probe, kWindow);
        const std::size_t nBusyMoved  = busyProbe.nMoved.load(std::memory_order_acquire) - nBusyBefore;
        std::println("idle parent passes over {} ms with a {} ms park: {} alone, {} with an idle managed subgraph, {} beside a busy unshared one that moved {} samples", kWindow.count(), kIdleParkMs, nAlone, nWithIdle, nWithBusy, nBusyMoved);
        expect(gt(nAlone, 0UZ)) << "the idle parent must still run its passes";
        expect(gt(nBusyMoved, 0UZ)) << "the busy subgraph must move samples";
        expect(le(nWithIdle, 2UZ * nAlone + 4UZ)) << "an idle subgraph must not wake the parked parent";
        expect(le(nWithBusy, 2UZ * nAlone + 4UZ)) << "a subgraph's work behind no shared ring must not wake the parked parent";
    };
};

int main() { /* tests are statically registered */ }
