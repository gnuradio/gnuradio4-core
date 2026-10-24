#include <boost/ut.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/LifeCycle.hpp>
#include <gnuradio-4.0/Message.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/thread/thread_pool.hpp>

namespace qa_start_stop {

// the block hooks the scheduler's child sweeps ran, counted per cycle
inline std::atomic<int> gStartHooks{0};
inline std::atomic<int> gStopHooks{0};

// hold start()'s child sweep open so that a stop can be placed inside it
inline std::atomic<bool> gSweepEntered{false};
inline std::atomic<bool> gSweepRelease{false};

struct ParkedStartSource : gr::Block<ParkedStartSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(ParkedStartSource, out);

    void start() {
        gStartHooks.fetch_add(1, std::memory_order_relaxed);
        gSweepEntered.store(true, std::memory_order_release);
        gSweepEntered.notify_all();
        gSweepRelease.wait(false, std::memory_order_acquire);
    }

    void stop() { gStopHooks.fetch_add(1, std::memory_order_relaxed); }

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }
};

struct CountingSink : gr::Block<CountingSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(CountingSink, in);

    void start() { gStartHooks.fetch_add(1, std::memory_order_relaxed); }

    void stop() { gStopHooks.fetch_add(1, std::memory_order_relaxed); }

    void processOne(float) {}
};

using TestScheduler = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>;

// the stop claims the scheduler's transition before its own child sweep runs, so this returns inside
// the window under test
[[nodiscard]] bool awaitShutdownClaim(const TestScheduler& scheduler) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!gr::lifecycle::isShuttingDown(scheduler.state())) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// the messages on kCountedEndpoint that reach a block
inline constexpr std::string_view kCountedEndpoint = "qa_counted";

inline std::size_t countCounted(std::span<const gr::Message> messages) { return static_cast<std::size_t>(std::ranges::count(messages, kCountedEndpoint, &gr::Message::endpoint)); }

// holds its worker inside processBulk() until the case releases it. A block reads its msgIn only between work() calls.
// The messages to the held block therefore stay unread
inline std::atomic<bool> gHeldEntered{false};
inline std::atomic<bool> gHeldRelease{false};

struct HeldSource : gr::Block<HeldSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(HeldSource, out);

    std::atomic<std::size_t> _nCounted{0UZ};

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        gHeldEntered.store(true, std::memory_order_release);
        gHeldRelease.wait(false, std::memory_order_acquire);
        outSpan.publish(0UZ);
        return gr::work::Status::OK;
    }

    void processMessages(const gr::MsgPortInBuiltin& port, std::span<const gr::Message> messages) {
        _nCounted.fetch_add(countCounted(messages), std::memory_order_relaxed);
        gr::BlockBase::processMessages(port, messages);
    }
};

// spends kSlowWork in each work() call and reads its msgIn between the calls
inline constexpr std::chrono::milliseconds kSlowWork{50};

struct SlowSource : gr::Block<SlowSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(SlowSource, out);

    std::atomic<std::size_t> _nCounted{0UZ};

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        std::this_thread::sleep_for(kSlowWork);
        outSpan.publish(0UZ);
        return gr::work::Status::OK;
    }

    void processMessages(const gr::MsgPortInBuiltin& port, std::span<const gr::Message> messages) {
        _nCounted.fetch_add(countCounted(messages), std::memory_order_relaxed);
        gr::BlockBase::processMessages(port, messages);
    }
};

struct MessageCountingSink : gr::Block<MessageCountingSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(MessageCountingSink, in);

    std::atomic<std::size_t> _nCounted{0UZ};

    void processOne(float) {}

    void processMessages(const gr::MsgPortInBuiltin& port, std::span<const gr::Message> messages) {
        _nCounted.fetch_add(countCounted(messages), std::memory_order_relaxed);
        gr::BlockBase::processMessages(port, messages);
    }
};

struct OnesSource : gr::Block<OnesSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(OnesSource, out);

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }
};

// the scheduler's running-job count, which the test reads without owning it
struct JobCountProbe : TestScheduler {
    using TestScheduler::TestScheduler;

    [[nodiscard]] std::weak_ptr<gr::Sequence> runningJobs() const { return this->_nRunningJobs; }
};

// a pool that holds every task it is given until runHeld() runs them on the calling thread. It keeps each task it ran
// until dropRan(), and it counts a held task as queued
struct HoldingPool : gr::thread_pool::TaskExecutor {
    std::string                                              _name;
    std::uint32_t                                            _maxThreads;
    mutable std::mutex                                       _mutex;
    std::vector<gr::thread_pool::detail::move_only_function> _held;
    std::vector<gr::thread_pool::detail::move_only_function> _ran;

    HoldingPool(std::string_view poolName, std::uint32_t maxThreads) : _name(poolName), _maxThreads(maxThreads) {}

    void execute(gr::thread_pool::detail::move_only_function&& task) override {
        std::lock_guard lock(_mutex);
        _held.push_back(std::move(task));
    }

    // returns the number of tasks it ran
    std::size_t runHeld() {
        std::vector<gr::thread_pool::detail::move_only_function> held;
        {
            std::lock_guard lock(_mutex);
            held.swap(_held);
        }
        for (auto& task : held) {
            task();
        }
        std::lock_guard lock(_mutex);
        for (auto& task : held) {
            _ran.push_back(std::move(task));
        }
        return held.size();
    }

    void dropRan() {
        std::lock_guard lock(_mutex);
        _ran.clear();
    }

    [[nodiscard]] gr::thread_pool::TaskType type() const noexcept override { return gr::thread_pool::TaskType::CPU_BOUND; }
    [[nodiscard]] std::string_view          name() const noexcept override { return _name; }
    [[nodiscard]] std::string_view          device() const noexcept override { return "CPU"; }
    [[nodiscard]] std::size_t               numThreads() const override { return 0UZ; }
    [[nodiscard]] std::size_t               numTasksQueued() const override {
        std::lock_guard lock(_mutex);
        return _held.size();
    }
    [[nodiscard]] std::size_t                   numTasksRunning() const override { return 0UZ; }
    [[nodiscard]] std::size_t                   numTasksRecycled() const override { return 0UZ; }
    void                                        setThreadBounds(std::uint32_t, std::uint32_t) override {}
    [[nodiscard]] std::pair<uint32_t, uint32_t> threadBounds() const override { return {0U, _maxThreads}; }
    [[nodiscard]] std::uint32_t                 minThreads() const override { return 0U; }
    [[nodiscard]] std::uint32_t                 maxThreads() const override { return _maxThreads; }
    void                                        requestShutdown() override {}
    [[nodiscard]] bool                          isShutdown() const override { return false; }
};

// registers a pool under its name for the case. At the end of the case the name resolves to the default CPU pool
struct ScopedPool {
    std::string                                    name;
    std::shared_ptr<gr::thread_pool::TaskExecutor> pool;

    ScopedPool(std::string_view poolName, std::shared_ptr<gr::thread_pool::TaskExecutor> namedPool) : name(poolName), pool(std::move(namedPool)) { gr::thread_pool::Manager::instance().replacePool(name, pool); }
    ScopedPool(const ScopedPool&)            = delete;
    ScopedPool& operator=(const ScopedPool&) = delete;
    ~ScopedPool() { gr::thread_pool::Manager::instance().replacePool(name, gr::thread_pool::Manager::defaultCpuPool()); }
};

template<typename TCondition>
[[nodiscard]] bool awaitCondition(TCondition condition, std::chrono::milliseconds bound = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// records the clientRequestID of each message on kCountedEndpoint that reaches it, in arrival order
struct MessageRecordingSink : gr::Block<MessageRecordingSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(MessageRecordingSink, in);

    std::mutex               _recordMutex;
    std::vector<std::string> _recorded;

    void processOne(float) {}

    void processMessages(const gr::MsgPortInBuiltin& port, std::span<const gr::Message> messages) {
        for (const gr::Message& msg : messages) {
            if (msg.endpoint == kCountedEndpoint) {
                std::lock_guard guard(_recordMutex);
                _recorded.push_back(msg.clientRequestID);
            }
        }
        gr::BlockBase::processMessages(port, messages);
    }

    [[nodiscard]] std::vector<std::string> recorded() {
        std::lock_guard guard(_recordMutex);
        return _recorded;
    }
};

// ends the run in its first work() call
struct DoneSource : gr::Block<DoneSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(DoneSource, out);

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        outSpan.publish(0UZ);
        return gr::work::Status::DONE;
    }
};

using SerialScheduler = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>;

// nMessages Set messages on kCountedEndpoint to serviceName. The clientRequestID of each is its index
inline std::vector<gr::Message> countedMessages(std::string_view serviceName, std::size_t nMessages) {
    std::vector<gr::Message> messages(nMessages);
    for (std::size_t i = 0UZ; i < nMessages; ++i) {
        messages[i].cmd             = gr::message::Command::Set;
        messages[i].serviceName     = serviceName;
        messages[i].endpoint        = kCountedEndpoint;
        messages[i].clientRequestID = std::to_string(i);
        messages[i].data            = gr::property_map{};
    }
    return messages;
}

inline std::vector<std::string> requestIds(std::span<const gr::Message> messages) {
    std::vector<std::string> ids;
    ids.reserve(messages.size());
    for (const gr::Message& msg : messages) {
        ids.push_back(msg.clientRequestID);
    }
    return ids;
}

} // namespace qa_start_stop

const boost::ut::suite<"stop requested during the start transient"> startStopRaceTests = [] {
    using namespace boost::ut;

    // the scheduler claims RUNNING before start() has moved a single child, so a caller that waits for
    // RUNNING and then stops is inside start()'s sweep by construction
    "a stop claimed inside start()'s child sweep starts every child or none"_test = [] {
        constexpr int nCycles = 64;

        std::size_t nCyclesPartlyStarted = 0UZ;
        std::size_t nCyclesLeftActive    = 0UZ;
        std::size_t nCyclesUnclaimed     = 0UZ;

        for (int cycle = 0; cycle < nCycles; ++cycle) {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_start_stop::ParkedStartSource>();
            auto&     sink   = flow.emplaceBlock<qa_start_stop::CountingSink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());

            qa_start_stop::gStartHooks.store(0, std::memory_order_release);
            qa_start_stop::gStopHooks.store(0, std::memory_order_release);
            qa_start_stop::gSweepEntered.store(false, std::memory_order_release);
            qa_start_stop::gSweepRelease.store(false, std::memory_order_release);

            qa_start_stop::TestScheduler scheduler({{"timeout_ms", gr::Size_t(5)}});
            expect(scheduler.exchange(std::move(flow)).has_value());

            std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });
            qa_start_stop::gSweepEntered.wait(false, std::memory_order_acquire);

            std::thread stopper([&scheduler] { scheduler.requestStop(); });
            if (!qa_start_stop::awaitShutdownClaim(scheduler)) {
                nCyclesUnclaimed++;
            }

            qa_start_stop::gSweepRelease.store(true, std::memory_order_release);
            qa_start_stop::gSweepRelease.notify_all();

            stopper.join();
            runner.join();

            if (qa_start_stop::gStartHooks.load(std::memory_order_acquire) != 2) {
                nCyclesPartlyStarted++;
            }
            if (gr::lifecycle::isActive(scheduler.state())) {
                nCyclesLeftActive++;
            }
        }

        expect(eq(nCyclesUnclaimed, 0UZ)) << "cycles in which the stop never claimed the scheduler's transition";
        expect(eq(nCyclesPartlyStarted, 0UZ)) << "cycles in which the stop left a child that start() could then not start";
        expect(eq(nCyclesLeftActive, 0UZ)) << "cycles that left the scheduler in an active state";
    };
};

const boost::ut::suite<"the scheduler's messages to its children"> childMessageTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    // the held block's worker reads nothing, and the other worker takes the scheduler's messages
    "a child held in work() leaves the scheduler taking messages and keeps one ring of them for later"_test = [] {
        constexpr std::string_view kPoolName = "qa_start_stop_two_cpu";
        qa_start_stop::gHeldEntered.store(false, std::memory_order_release);
        qa_start_stop::gHeldRelease.store(false, std::memory_order_release);
        qa_start_stop::ScopedPool pool(kPoolName, std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 2U, 2U), "CPU"));

        gr::Graph flow;
        auto&     held = flow.emplaceBlock<qa_start_stop::HeldSource>();
        auto&     sink = flow.emplaceBlock<qa_start_stop::MessageCountingSink>();
        expect(flow.connect<"out", "in">(held, sink).has_value());

        qa_start_stop::TestScheduler scheduler({{"poolName", std::string(kPoolName)}});
        expect(scheduler.exchange(std::move(flow)).has_value());
        gr::MsgPortOut toScheduler;
        expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(fatal(qa_start_stop::awaitCondition([] { return qa_start_stop::gHeldEntered.load(std::memory_order_acquire); }))) << "the held block never entered work()";

        // more messages than the children's ring and the scheduler's store hold together, addressed to no block
        const std::size_t ringSize      = held.msgIn.buffer().streamBuffer.size();
        const std::size_t unreadBefore  = held.msgIn.streamReader().available();
        const std::size_t nMessages     = 2UZ * ringSize + 3UZ;
        const std::size_t droppedBefore = gr::message::droppedMessageCount().load();
        bool              allTaken      = true;
        for (std::size_t nSent = 0UZ; nSent < nMessages && allTaken;) {
            const std::size_t nBatch = std::min(nMessages - nSent, toScheduler.streamWriter().available());
            for (std::size_t i = 0UZ; i < nBatch; ++i) {
                gr::sendMessage<gr::message::Command::Set>(toScheduler, "qa_no_such_block", qa_start_stop::kCountedEndpoint, gr::property_map{});
            }
            nSent += nBatch;
            allTaken = qa_start_stop::awaitCondition([&scheduler] { return scheduler.msgIn.streamReader().available() == 0UZ; });
        }
        const std::size_t nDropped = gr::message::droppedMessageCount().load() - droppedBefore;

        qa_start_stop::gHeldRelease.store(true, std::memory_order_release);
        qa_start_stop::gHeldRelease.notify_all();

        // the ring takes what its slowest reader left room for, the scheduler keeps one ring more, and it drops the rest
        const std::size_t nDelivered = nMessages - nDropped;
        expect(allTaken) << "the scheduler stopped taking messages once the held block's msgIn was full";
        expect(eq(nDropped, nMessages - (ringSize - unreadBefore) - ringSize)) << "the scheduler drops only the messages past the ring and one ring kept";
        expect(qa_start_stop::awaitCondition([&] { return held._nCounted.load() == nDelivered && sink._nCounted.load() == nDelivered; })) << "the kept messages never reached the children";
        expect(eq(held._nCounted.load(), nDelivered)) << "the held block's deliveries";
        expect(eq(sink._nCounted.load(), nDelivered)) << "the other block's deliveries";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_start_stop::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    // a burst larger than the free room in the ring reaches every child once the slow block reads its messages
    "a child slow in work() costs no child its messages"_test = [] {
        constexpr std::string_view kPoolName = "qa_start_stop_two_cpu";
        constexpr std::size_t      nMessages = 5000UZ;
        qa_start_stop::ScopedPool  pool(kPoolName, std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kPoolName, gr::thread_pool::TaskType::CPU_BOUND, 2U, 2U), "CPU"));

        gr::Graph flow;
        auto&     slow = flow.emplaceBlock<qa_start_stop::SlowSource>();
        auto&     sink = flow.emplaceBlock<qa_start_stop::MessageCountingSink>();
        expect(flow.connect<"out", "in">(slow, sink).has_value());

        qa_start_stop::TestScheduler scheduler({{"poolName", std::string(kPoolName)}});
        expect(scheduler.exchange(std::move(flow)).has_value());
        gr::MsgPortOut toScheduler;
        expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        const std::size_t droppedBefore = gr::message::droppedMessageCount().load();
        std::size_t       nSent         = 0UZ;
        const bool        allQueued     = qa_start_stop::awaitCondition([&] {
            const std::size_t nBatch = std::min(nMessages - nSent, toScheduler.streamWriter().available());
            for (std::size_t i = 0UZ; i < nBatch; ++i) {
                gr::sendMessage<gr::message::Command::Set>(toScheduler, "qa_no_such_block", qa_start_stop::kCountedEndpoint, gr::property_map{});
            }
            nSent += nBatch;
            return nSent == nMessages;
        });
        expect(allQueued) << "the scheduler stopped taking messages";
        expect(qa_start_stop::awaitCondition([&] { return slow._nCounted.load() == nSent && sink._nCounted.load() == nSent; })) << "a child missed messages";
        expect(eq(slow._nCounted.load(), nSent)) << "the slow block's deliveries";
        expect(eq(sink._nCounted.load(), nSent)) << "the other block's deliveries";
        expect(eq(gr::message::droppedMessageCount().load() - droppedBefore, 0UZ)) << "no message may be dropped";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_start_stop::awaitCondition([&scheduler] { return scheduler.state() == STOPPED; })) << "the scheduler did not stop";
    };

    // the graph reads the children's ring as a child does. A reconnection takes what the graph left unread and sends
    // it again
    "a reconnection forwards the graph's unread messages before the kept ones"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_start_stop::DoneSource>();
        auto&     sink   = flow.emplaceBlock<qa_start_stop::MessageRecordingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_start_stop::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(fatal(scheduler.runAndWait().has_value()));
        expect(fatal(scheduler.state() == STOPPED));
        expect(fatal(eq(scheduler.graph().msgIn.streamReader().available(), 0UZ)));
        expect(fatal(eq(sink.msgIn.streamReader().available(), 0UZ)));

        // no pass runs: the ring takes its size in messages, and the scheduler keeps the rest
        constexpr std::size_t          nKept         = 3UZ;
        const std::size_t              ringSize      = sink.msgIn.buffer().streamBuffer.size();
        const std::vector<gr::Message> messages      = qa_start_stop::countedMessages("qa_no_such_block", ringSize + nKept);
        const std::size_t              droppedBefore = gr::message::droppedMessageCount().load();
        scheduler.processMessages(scheduler.msgIn, messages);
        expect(fatal(eq(scheduler.graph().msgIn.streamReader().available(), ringSize))) << "the graph's unread messages";

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        // the first pass delivers the ring. The second sends and delivers the messages the ring could not take
        scheduler.processScheduledMessages();
        scheduler.processScheduledMessages();
        expect(sink.recorded() == qa_start_stop::requestIds(messages)) << "the sink received the messages out of order";
        expect(eq(gr::message::droppedMessageCount().load() - droppedBefore, 0UZ)) << "no message may be dropped";
    };

    "a graph swap drops the kept messages, and none reaches a block of the new graph"_test = [] {
        constexpr std::string_view kBlockName = "qa_named";
        gr::Graph                  oldFlow;
        auto&                      oldSource = oldFlow.emplaceBlock<qa_start_stop::DoneSource>();
        auto&                      oldSink   = oldFlow.emplaceBlock<qa_start_stop::MessageRecordingSink>({{"name", std::string(kBlockName)}});
        expect(oldFlow.connect<"out", "in">(oldSource, oldSink).has_value());

        qa_start_stop::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(oldFlow)).has_value());
        expect(fatal(scheduler.changeStateTo(INITIALISED).has_value()));
        expect(fatal(eq(oldSink.msgIn.streamReader().available(), 0UZ)));

        const std::size_t              ringSize      = oldSink.msgIn.buffer().streamBuffer.size();
        const std::vector<gr::Message> messages      = qa_start_stop::countedMessages(kBlockName, ringSize + 3UZ);
        const std::size_t              droppedBefore = gr::message::droppedMessageCount().load();
        scheduler.processMessages(scheduler.msgIn, messages);
        expect(fatal(eq(gr::message::droppedMessageCount().load() - droppedBefore, 0UZ)));

        gr::Graph newFlow;
        auto&     newSource = newFlow.emplaceBlock<qa_start_stop::DoneSource>();
        auto&     newSink   = newFlow.emplaceBlock<qa_start_stop::MessageRecordingSink>({{"name", std::string(kBlockName)}});
        expect(newFlow.connect<"out", "in">(newSource, newSink).has_value());
        expect(scheduler.exchange(std::move(newFlow)).has_value()); // the old graph is released with the returned value

        scheduler.processScheduledMessages();
        scheduler.processScheduledMessages();
        expect(newSink.recorded().empty()) << "a message kept for the old graph reached the new graph's block of the same name";
        expect(eq(gr::message::droppedMessageCount().load() - droppedBefore, messages.size() - ringSize)) << "the swap counts each kept message as dropped";
    };

    // the graph reads its msgIn, as a running graph does in each pass
    "a graph swap of a stopped scheduler counts each kept message after the graph has read its messages"_test = [] {
        gr::Graph oldFlow;
        auto&     oldSource = oldFlow.emplaceBlock<qa_start_stop::DoneSource>();
        auto&     oldSink   = oldFlow.emplaceBlock<qa_start_stop::MessageRecordingSink>();
        expect(oldFlow.connect<"out", "in">(oldSource, oldSink).has_value());

        qa_start_stop::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(oldFlow)).has_value());
        expect(fatal(scheduler.runAndWait().has_value()));
        expect(fatal(scheduler.state() == STOPPED));

        const std::size_t              ringSize      = oldSink.msgIn.buffer().streamBuffer.size();
        const std::vector<gr::Message> messages      = qa_start_stop::countedMessages("qa_no_such_block", ringSize + 3UZ);
        const std::size_t              droppedBefore = gr::message::droppedMessageCount().load();
        scheduler.processMessages(scheduler.msgIn, messages);
        {
            auto graphMessages = scheduler.graph().msgIn.streamReader().get<gr::SpanReleasePolicy::ProcessAll>(ringSize);
            expect(fatal(eq(graphMessages.size(), ringSize))) << "the graph read fewer messages than the ring holds";
        }

        gr::Graph newFlow;
        auto&     newSource = newFlow.emplaceBlock<qa_start_stop::DoneSource>();
        auto&     newSink   = newFlow.emplaceBlock<qa_start_stop::MessageRecordingSink>();
        expect(newFlow.connect<"out", "in">(newSource, newSink).has_value());
        expect(scheduler.exchange(std::move(newFlow)).has_value());
        const std::size_t nDropped = gr::message::droppedMessageCount().load() - droppedBefore;

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        scheduler.processScheduledMessages();
        scheduler.processScheduledMessages();
        expect(newSink.recorded().empty()) << "the new graph's block received a message kept for the old graph";
        expect(eq(nDropped, messages.size() - ringSize)) << "the swap left kept messages uncounted";
    };
};

const boost::ut::suite<"a worker task queued for a stopped run"> queuedWorkerTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    // the pool runs the stopped run's worker tasks only after the stop. Each task then releases its count and returns
    "a queued worker task keeps the job count it releases alive after the scheduler is gone"_test = [] {
        constexpr std::string_view kPoolName = "qa_start_stop_holding_cpu";
        auto                       holding   = std::make_shared<qa_start_stop::HoldingPool>(kPoolName, 4U);
        qa_start_stop::ScopedPool  pool(kPoolName, holding);

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_start_stop::OnesSource>();
        auto&     sink   = flow.emplaceBlock<qa_start_stop::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        auto scheduler = std::make_unique<qa_start_stop::JobCountProbe>(gr::property_map{{"poolName", std::string(kPoolName)}});
        expect(scheduler->exchange(std::move(flow)).has_value());
        expect(scheduler->changeStateTo(INITIALISED).has_value());
        expect(scheduler->changeStateTo(RUNNING).has_value());
        expect(fatal(gt(holding->numTasksQueued(), 0UZ))) << "the run's workers must be held";
        expect(scheduler->changeStateTo(REQUESTED_STOP).has_value());
        expect(fatal(qa_start_stop::awaitCondition([&scheduler] { return scheduler->state() == STOPPED; }))) << "the scheduler did not stop with its workers held";

        const std::weak_ptr<gr::Sequence> runningJobs = scheduler->runningJobs();
        expect(gt(holding->runHeld(), 0UZ));
        expect(!scheduler->workerStarted()) << "a worker task of the stopped run must leave without starting";
        expect(fatal(qa_start_stop::awaitCondition([&scheduler] { return !scheduler->isProcessing(); }))) << "the stopped run's workers kept their count";
        scheduler.reset();

        expect(!runningJobs.expired()) << "a worker task must hold the job count it releases";
        holding->dropRan();
        expect(runningJobs.expired()) << "the job count must go with the last task that holds it";
    };
};

int main() { /* tests are statically registered */ }
