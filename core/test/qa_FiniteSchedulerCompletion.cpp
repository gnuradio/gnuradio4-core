#include <atomic>
#include <boost/ut.hpp>
#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <limits>
#include <thread>
#include <type_traits>
namespace finite_scheduler_completion {
struct Source : gr::Block<Source> {
    gr::PortIn<int, gr::Optional> clock;
    gr::PortOut<int>              out, second;
    GR_MAKE_REFLECTABLE(Source, clock, out, second);
    std::size_t              limit = 20, produced = 0;
    std::atomic<bool>        entered     = false;
    std::atomic<std::size_t> starts      = 0;
    bool                     failOnStart = false;
    bool                     waitForStop = false;
    std::atomic<bool>        stopped     = false;
    [[nodiscard]] bool       isBlocking() const noexcept { return waitForStop; }
    std::function<void()>    onStop;
    void                     stop() {
        stopped = true;
        if (onStop) {
            onStop();
        }
    }
    void start() {
        ++starts;
        if (failOnStart) {
            throw gr::exception("source refused to start");
        }
        produced = 0;
    }
    gr::work::Status processBulk(gr::InputSpanLike auto&, gr::OutputSpanLike auto& a, gr::OutputSpanLike auto& b) {
        entered = true;
        while (waitForStop && !stopped.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (produced == limit) {
            a.publish(0);
            b.publish(0);
            return gr::work::Status::DONE;
        }
        const auto n = std::min({a.size(), b.size(), limit - produced});
        for (std::size_t i = 0; i < n; ++i) {
            a[i] = b[i] = int(produced + i);
        }
        produced += n;
        a.publish(n);
        b.publish(n);
        return produced == limit ? gr::work::Status::DONE : gr::work::Status::OK;
    }
};
struct Sink : gr::Block<Sink> {
    gr::PortIn<int> in;
    GR_MAKE_REFLECTABLE(Sink, in);
    std::size_t        consumed    = 0;
    bool               stall       = false;
    bool               failOnWork  = false;
    std::size_t        maxConsume  = std::numeric_limits<std::size_t>::max();
    std::atomic<bool>  entered     = false;
    bool               waitForStop = false;
    std::atomic<bool>  stopped     = false;
    [[nodiscard]] bool isBlocking() const noexcept { return waitForStop; }
    void               stop() { stopped = true; }
    gr::work::Status   processBulk(gr::InputSpanLike auto& input) {
        entered = true;
        while (waitForStop && !stopped.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (failOnWork) {
            return gr::work::Status::ERROR;
        }
        if (stall) {
            std::ignore = input.consume(0);
            return gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        const auto n = std::min(input.size(), maxConsume);
        consumed += n;
        std::ignore = input.consume(n);
        return gr::work::Status::OK;
    }
};
template<gr::scheduler::ExecutionPolicy Policy>
struct ThrowingInitScheduler : gr::scheduler::SchedulerBase<ThrowingInitScheduler<Policy>, Policy> {
    using gr::scheduler::SchedulerBase<ThrowingInitScheduler<Policy>, Policy>::SchedulerBase;
    bool failDuringInit = true;
    void customStart() {
        if (!failDuringInit) {
            throw gr::exception("scheduler startup refused");
        }
    }
    void arm() { this->_jobListsPool = nullptr; }
    void customInit() {
        if (failDuringInit && this->state() == gr::lifecycle::State::RUNNING) {
            throw gr::exception("scheduler startup refused");
        }
        const auto flat = gr::graph::flatten(*this->_graph);
        this->_adoptionBlocks.resize(1);
        *this->_executionOrder = gr::scheduler::detail::batchBlocks(flat.blocks(), 1);
    }
};

template<gr::scheduler::ExecutionPolicy Policy>
struct CountingDrainScheduler : gr::scheduler::Simple<Policy> {
    using gr::scheduler::Simple<Policy>::Simple;
    std::atomic<std::size_t> passes = 0;
    bool                     cancelDrainSources(std::chrono::steady_clock::time_point deadline) {
        ++passes;
        return gr::scheduler::Simple<Policy>::cancelDrainSources(deadline);
    }
};

template<gr::scheduler::ExecutionPolicy Policy>
struct DrainEditScheduler : gr::scheduler::Simple<Policy> {
    using gr::scheduler::Simple<Policy>::Simple;

    // Expose a graph-edit attempt at a controlled thread boundary. A busy
    // dispatcher leaves the request pending, as processScheduledMessages() does.
    std::optional<std::expected<gr::property_map, gr::Error>> tryRemoveEdge(const gr::property_map& edge) {
        if (!this->tryHoldMessagePass()) {
            return std::nullopt;
        }
        gr::on_scope_exit release = [this] { this->releaseMessagePass(); };
        gr::Message       request;
        request.data = edge;
        return this->removeEdgeByMessage(request);
    }
};

template<gr::scheduler::ExecutionPolicy Policy>
struct EntryGateScheduler : gr::scheduler::Simple<Policy> {
    using Base = gr::scheduler::Simple<Policy>;

    std::atomic<bool> entryHeld    = false;
    std::atomic<bool> releaseEntry = false;

    explicit EntryGateScheduler(const gr::property_map& settings = {}) : Base(settings) { this->propertyCallbacks["holdAsyncEntry"] = static_cast<gr::BlockBase::PropertyCallback>(&EntryGateScheduler::holdAsyncEntry); }

    std::optional<gr::Message> holdAsyncEntry(std::string_view, gr::Message) {
        entryHeld = true;
        releaseEntry.wait(false);
        return std::nullopt;
    }
};

template<gr::scheduler::ExecutionPolicy Policy>
struct StartupGateScheduler : gr::scheduler::SchedulerBase<StartupGateScheduler<Policy>, Policy> {
    std::atomic<bool> entered  = false;
    std::atomic<bool> released = false;
    void              arm() { this->_jobListsPool = nullptr; }
    void              customInit() {
        if (this->state() == gr::lifecycle::State::RUNNING) {
            entered = true;
            while (!released) {
                std::this_thread::yield();
            }
        }
        const auto flat = gr::graph::flatten(*this->_graph);
        this->_adoptionBlocks.clear();
        this->_adoptionBlocks.resize(1);
        *this->_executionOrder = gr::scheduler::detail::batchBlocks(flat.blocks(), 1);
    }
};

bool wait(const auto& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}
} // namespace finite_scheduler_completion
template<gr::scheduler::ExecutionPolicy Policy>
void qualify() {
    using namespace boost::ut;
    using namespace finite_scheduler_completion;
    const auto policyTest = [](std::string_view name) {
        static std::deque<std::string> names;
        names.push_back(std::format("{} / {}", name, gr::meta::enumName(Policy).value_or("unknown")));
        return boost::ut::test(std::string_view{names.back()});
    };
    policyTest("drain releases blocking sources and times out blocking consumers, including nested graphs") = [] {
        for (bool nested : {false, true}) {
            for (bool blockingSource : {false, true}) {
                gr::Graph flow;
                auto&     source   = flow.emplaceBlock<Source>();
                source.limit       = std::size_t(-1);
                source.waitForStop = blockingSource;
                auto& sink         = flow.emplaceBlock<Sink>();
                sink.waitForStop   = !blockingSource;
                expect(flow.connect<"out", "in">(source, sink).has_value());
                gr::SchedulerWrapper<gr::scheduler::Simple<Policy>> scheduler;
                if (nested) {
                    auto inner = std::make_shared<gr::SchedulerWrapper<gr::scheduler::Simple<Policy>>>();
                    inner->setGraph(std::move(flow));
                    gr::Graph outer;
                    outer.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
                    scheduler.setGraph(std::move(outer));
                } else {
                    scheduler.setGraph(std::move(flow));
                }
                expect(scheduler.start().has_value());
                expect(wait([&] { return blockingSource ? source.entered.load() : sink.entered.load(); }));
                auto       drain     = std::async(std::launch::async, [&] { return scheduler.stopAfterDrain(blockingSource ? std::chrono::milliseconds(2000) : std::chrono::milliseconds(50)); });
                const bool completed = drain.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
                expect(completed);
                if (!completed) {
                    source.stopped = true;
                    sink.stopped   = true;
                }
                expect(eq(drain.get(), blockingSource));
                if (blockingSource) {
                    expect(eq(source.produced, sink.consumed));
                }
            }
        }
    };
    policyTest("draining consumers does not repeat source cancellation") = [] {
        std::atomic<bool> cancelled = false;
        gr::Graph         graph;
        auto&             source = graph.emplaceBlock<Source>();
        source.limit             = std::size_t(-1);
        auto& sink               = graph.emplaceBlock<Sink>();
        sink.stall               = true;
        source.onStop            = [&] {
            sink.stall       = false;
            sink.entered     = false;
            sink.waitForStop = true;
            cancelled        = true;
        };
        expect(graph.connect<"out", "in">(source, sink).has_value());
        auto inner = std::make_shared<gr::SchedulerWrapper<CountingDrainScheduler<Policy>>>();
        inner->setGraph(std::move(graph));
        gr::Graph outer;
        outer.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
        gr::SchedulerWrapper<gr::scheduler::Simple<Policy>> scheduler;
        scheduler.setGraph(std::move(outer));
        expect(scheduler.start().has_value());
        expect(wait([&] { return sink.entered.load(); }));
        auto drain = std::async(std::launch::async, [&] { return scheduler.stopAfterDrain(std::chrono::seconds(5)); });
        expect(wait([&] { return cancelled.load() && sink.entered.load(); }));
        const auto passes = inner->blockRef().passes.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        expect(eq(inner->blockRef().passes.load(), passes));
        sink.stopped = true;
        expect(drain.get());
    };
    policyTest("an error during drain is not successful completion") = [] {
        gr::Graph graph;
        auto&     source = graph.emplaceBlock<Source>();
        source.limit     = std::size_t(-1);
        auto& sink       = graph.emplaceBlock<Sink>();
        sink.stall       = true;
        source.onStop    = [&] {
            sink.stall      = false;
            sink.failOnWork = true;
        };
        expect(graph.connect<"out", "in">(source, sink).has_value());
        gr::SchedulerWrapper<gr::scheduler::Simple<Policy>> scheduler;
        scheduler.setGraph(std::move(graph));
        gr::MsgPortIn messages;
        expect(scheduler.asBlockModel()->msgOut->connect(messages).has_value());
        expect(scheduler.start().has_value());
        expect(wait([&] { return sink.entered.load(); }));
        expect(!scheduler.stopAfterDrain(std::chrono::seconds(5)));
    };
    policyTest("scheduler startup exceptions are retained and children are stopped") = [] {
        for (bool duringInit : {true, false}) {
            gr::SchedulerWrapper<ThrowingInitScheduler<Policy>> scheduler;
            gr::Graph                                           graph;
            auto&                                               source = graph.emplaceBlock<Source>();
            auto&                                               sink   = graph.emplaceBlock<Sink>();
            expect(graph.connect<"out", "in">(source, sink).has_value());
            scheduler.setGraph(std::move(graph));
            scheduler.blockRef().failDuringInit = duringInit;
            expect(scheduler.blockRef().changeStateTo(gr::lifecycle::State::INITIALISED).has_value());
            scheduler.blockRef().arm();
            gr::MsgPortIn messages;
            expect(scheduler.asBlockModel()->msgOut->connect(messages).has_value());
            expect(scheduler.start().has_value());
            expect(wait([&] { return scheduler.state() == gr::lifecycle::State::ERROR; }));
            scheduler.stop();
            expect(scheduler.startError().has_value());
            expect(!gr::lifecycle::isActive(source.state()));
            expect(!gr::lifecycle::isActive(sink.state()));
            auto replies = messages.streamReader().get(messages.streamReader().available());
            expect(eq(std::ranges::count_if(replies, [](const auto& m) { return m.endpoint == "start()"; }), std::ptrdiff_t{1}));
        }
    };
    policyTest("async finite completion settles state and drains both output ports") = [] {
        for (const std::size_t count : {1UZ, 20UZ, 10000UZ}) {
            gr::Graph graph;
            auto&     src = graph.emplaceBlock<Source>();
            src.limit     = count;
            auto& a       = graph.emplaceBlock<Sink>();
            auto& b       = graph.emplaceBlock<Sink>();
            expect(src.out.resizeBuffer(4).has_value());
            expect(src.second.resizeBuffer(4).has_value());
            expect(graph.connect<"out", "in">(src, a).has_value());
            expect(graph.connect<"second", "in">(src, b).has_value());
            gr::SchedulerWrapper<gr::scheduler::Simple<Policy>> scheduler;
            scheduler.setGraph(std::move(graph));
            expect(scheduler.start().has_value());
            expect(wait([&] { return scheduler.state() == gr::lifecycle::State::STOPPED; }));
            scheduler.stop();
            expect(eq(a.consumed, std::size_t(count)));
            expect(eq(b.consumed, std::size_t(count)));
            expect(scheduler.start().has_value());
            expect(wait([&] { return scheduler.state() == gr::lifecycle::State::STOPPED; }));
            scheduler.stop();
            expect(eq(a.consumed, 2UZ * count));
            expect(eq(b.consumed, 2UZ * count));
        }
    };
    policyTest("drain stop cancels sources and consumes accepted data; stalled drain times out") = [] {
        for (const bool stall : {false, true}) {
            gr::Graph graph;
            auto&     src = graph.emplaceBlock<Source>();
            src.limit     = std::size_t(-1);
            auto& a       = graph.emplaceBlock<Sink>();
            auto& b       = graph.emplaceBlock<Sink>();
            a.stall       = stall;
            expect(src.out.resizeBuffer(4).has_value());
            expect(src.second.resizeBuffer(4).has_value());
            expect(graph.connect<"out", "in">(src, a).has_value());
            expect(graph.connect<"second", "in">(src, b).has_value());
            gr::SchedulerWrapper<gr::scheduler::Simple<Policy>> scheduler;
            scheduler.setGraph(std::move(graph));
            expect(scheduler.start().has_value());
            expect(wait([&] { return a.entered.load(); }));
            expect(eq(scheduler.stopAfterDrain(stall ? std::chrono::milliseconds(50) : std::chrono::milliseconds(5000)), !stall));
            if (!stall) {
                expect(eq(src.produced, a.consumed));
                expect(eq(src.produced, b.consumed));
            }
            expect(scheduler.state() == gr::lifecycle::State::STOPPED);
        }
    };
    policyTest("drain stop quiesces a nested scheduler before cancelling its source") = [] {
        gr::Graph innerGraph;
        auto&     source = innerGraph.emplaceBlock<Source>();
        source.limit     = std::size_t(-1);
        auto& first      = innerGraph.emplaceBlock<Sink>();
        auto& second     = innerGraph.emplaceBlock<Sink>();
        expect(innerGraph.connect<"out", "in">(source, first).has_value());
        expect(innerGraph.connect<"second", "in">(source, second).has_value());
        auto inner = std::make_shared<gr::SchedulerWrapper<gr::scheduler::Simple<Policy>>>();
        inner->setGraph(std::move(innerGraph));
        gr::Graph graph;
        graph.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
        gr::SchedulerWrapper<gr::scheduler::Simple<Policy>> scheduler;
        scheduler.setGraph(std::move(graph));
        expect(scheduler.start().has_value());
        expect(wait([&] { return first.entered.load(); }));
        expect(scheduler.stopAfterDrain(std::chrono::seconds(2)));
        expect(eq(source.produced, first.consumed));
        expect(eq(source.produced, second.consumed));
        expect(scheduler.state() == gr::lifecycle::State::STOPPED);
    };
    policyTest("drain excludes graph edits until source cancellation has finished") = [] {
        for (const bool nested : {false, true}) {
            std::atomic<bool> sourceStopping = false;
            std::atomic<bool> releaseStop    = false;
            gr::Graph         graph;
            auto&             source = graph.emplaceBlock<Source>();
            source.limit             = std::numeric_limits<std::size_t>::max();
            source.onStop            = [&] {
                sourceStopping = true;
                releaseStop.wait(false);
            };
            auto& first   = graph.emplaceBlock<Sink>();
            auto& second  = graph.emplaceBlock<Sink>();
            auto& removed = graph.emplaceBlock<Sink>();
            expect(graph.connect<"out", "in">(source, first).has_value());
            expect(graph.connect<"second", "in">(source, second).has_value());
            expect(graph.connect<"out", "in">(source, removed).has_value());
            using Scheduler = gr::SchedulerWrapper<DrainEditScheduler<Policy>>;
            auto      inner = std::make_shared<Scheduler>();
            Scheduler scheduler;
            auto*     edited = &scheduler.blockRef();
            if (nested) {
                inner->setGraph(std::move(graph));
                gr::Graph outer;
                outer.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
                scheduler.setGraph(std::move(outer));
                edited = &inner->blockRef();
            } else {
                scheduler.setGraph(std::move(graph));
            }
            expect(scheduler.start().has_value());
            expect(wait([&] { return first.entered.load(); }));
            auto drain = std::async(std::launch::async, [&] { return scheduler.stopAfterDrain(std::chrono::seconds(2)); });
            expect(wait([&] { return sourceStopping.load(); }));
            using namespace gr::serialization_fields;
            const gr::property_map edge{{std::pmr::string(EDGE_SOURCE_BLOCK), std::string(source.unique_name)}, {std::pmr::string(EDGE_SOURCE_PORT), std::string("out")}, {std::pmr::string(EDGE_DESTINATION_BLOCK), std::string(removed.unique_name)}, {std::pmr::string(EDGE_DESTINATION_PORT), std::string("in")}};
            // Enter the real edge-edit handler from another controller, using
            // the same message-pass claim as processScheduledMessages().
            auto edit = std::async(std::launch::async, [&] { return edited->tryRemoveEdge(edge); });
            expect(!edit.get().has_value()) << "the graph edit ran inside the held cancellation pass";
            releaseStop = true;
            releaseStop.notify_all();
            expect(drain.get());
            expect(eq(source.produced, first.consumed));
            expect(eq(source.produced, second.consumed));
            const auto response = edited->tryRemoveEdge(edge);
            expect(response.has_value() && response->has_value());
            if (response && *response) {
                expect(eq(response->value().at("nEdgesRemoved").template value_or<gr::Size_t>(0), gr::Size_t{1}));
            }
        }
    };
    policyTest("an asynchronous run error is not also a start failure") = [] {
        gr::Graph graph;
        auto&     source = graph.emplaceBlock<Source>();
        source.limit     = std::size_t(-1);
        auto& first      = graph.emplaceBlock<Sink>();
        first.failOnWork = true;
        auto& second     = graph.emplaceBlock<Sink>();
        expect(graph.connect<"out", "in">(source, first).has_value());
        expect(graph.connect<"second", "in">(source, second).has_value());
        gr::SchedulerWrapper<gr::scheduler::Simple<Policy>> scheduler;
        scheduler.setGraph(std::move(graph));
        gr::MsgPortIn messages;
        expect(scheduler.asBlockModel()->msgOut->connect(messages).has_value());
        expect(scheduler.start().has_value());
        expect(wait([&] { return scheduler.state() == gr::lifecycle::State::ERROR; }));
        scheduler.stop();
        expect(!scheduler.startError().has_value());
        auto replies = messages.streamReader().get(messages.streamReader().available());
        expect(eq(std::ranges::count_if(replies, [](const auto& m) { return m.endpoint == "error in the run"; }), std::ptrdiff_t{1}));
        expect(eq(std::ranges::count_if(replies, [](const auto& m) { return m.endpoint == "start()"; }), std::ptrdiff_t{0}));
    };
    policyTest("a refused asynchronous start keeps its startup error") = [] {
        gr::Graph graph;
        auto&     source   = graph.emplaceBlock<Source>();
        source.failOnStart = true;
        auto& first        = graph.emplaceBlock<Sink>();
        auto& second       = graph.emplaceBlock<Sink>();
        expect(graph.connect<"out", "in">(source, first).has_value());
        expect(graph.connect<"second", "in">(source, second).has_value());
        gr::SchedulerWrapper<gr::scheduler::Simple<Policy>> scheduler;
        scheduler.setGraph(std::move(graph));
        expect(scheduler.start().has_value());
        expect(wait([&] { return scheduler.state() == gr::lifecycle::State::ERROR; }));
        scheduler.stop();
        expect(scheduler.startError().has_value());
    };
    policyTest("drain waits for slow consumers on externally driven wrapped and bare runs") = [] {
        auto check = []<typename Scheduler>(Scheduler& scheduler) {
            gr::Graph graph;
            auto&     source  = graph.emplaceBlock<Source>();
            source.limit      = std::size_t(-1);
            auto& first       = graph.emplaceBlock<Sink>();
            first.maxConsume  = 1;
            auto& second      = graph.emplaceBlock<Sink>();
            second.maxConsume = 1;
            expect(source.out.resizeBuffer(64).has_value());
            expect(source.second.resizeBuffer(64).has_value());
            expect(graph.connect<"out", "in">(source, first).has_value());
            expect(graph.connect<"second", "in">(source, second).has_value());
            if constexpr (requires { scheduler.setGraph(std::move(graph)); }) {
                scheduler.setGraph(std::move(graph));
            } else {
                expect(scheduler.exchange(std::move(graph)).has_value());
            }
            std::expected<void, gr::Error> result;
            std::thread                    runner([&] { result = scheduler.runAndWait(); });
            expect(wait([&] { return first.entered.load(); }));
            expect(scheduler.stopAfterDrain(std::chrono::seconds(2)));
            runner.join();
            expect(result.has_value());
            expect(gt(source.produced, 1UZ));
            expect(eq(source.produced, first.consumed));
            expect(eq(source.produced, second.consumed));
        };
        gr::SchedulerWrapper<gr::scheduler::Simple<Policy>> wrapped;
        check(wrapped);
        gr::scheduler::Simple<Policy> bare;
        check(bare);
        static_assert(!std::is_invocable_v<decltype(&gr::scheduler::Simple<Policy>::runAndWait), gr::scheduler::Simple<Policy>&, bool>);
    };
    policyTest("drain retries while RUNNING is published but sources have not started") = [] {
        gr::Graph graph;
        auto&     source = graph.emplaceBlock<Source>();
        source.limit     = std::size_t(-1);
        auto& first      = graph.emplaceBlock<Sink>();
        auto& second     = graph.emplaceBlock<Sink>();
        expect(graph.connect<"out", "in">(source, first).has_value());
        expect(graph.connect<"second", "in">(source, second).has_value());
        StartupGateScheduler<Policy> scheduler;
        expect(scheduler.exchange(std::move(graph)).has_value());
        expect(scheduler.changeStateTo(gr::lifecycle::State::INITIALISED).has_value());
        scheduler.arm();
        std::expected<void, gr::Error> result;
        std::thread                    runner([&] { result = scheduler.runAndWait(); });
        expect(wait([&] { return scheduler.entered.load(); }));
        expect(source.state() == gr::lifecycle::State::INITIALISED);
        std::atomic<bool> draining = false;
        bool              drained  = false;
        std::thread       controller([&] {
            draining = true;
            drained  = scheduler.stopAfterDrain(std::chrono::seconds(2));
        });
        expect(wait([&] { return draining.load(); }));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        scheduler.released = true;
        controller.join();
        runner.join();
        expect(drained);
        expect(result.has_value());
        expect(eq(source.produced, first.consumed));
        expect(eq(source.produced, second.consumed));
    };
    policyTest("plain stop does not publish drain EOS and its graph restarts") = [] {
        gr::Graph graph;
        auto&     source = graph.emplaceBlock<Source>();
        source.limit     = std::size_t(-1);
        auto& first      = graph.emplaceBlock<Sink>();
        first.stall      = true;
        auto& second     = graph.emplaceBlock<Sink>();
        expect(graph.connect<"out", "in">(source, first).has_value());
        expect(graph.connect<"second", "in">(source, second).has_value());
        gr::SchedulerWrapper<gr::scheduler::Simple<Policy>> scheduler;
        scheduler.setGraph(std::move(graph));
        for (int run = 0; run < 3; ++run) {
            first.entered = false;
            expect(scheduler.start().has_value());
            expect(wait([&] { return first.entered.load(); }));
            scheduler.stop();
            auto tags = first.in.tagReader().get(first.in.tagReader().available());
            expect(std::ranges::none_of(tags, [](const auto& streamTag) { return streamTag.map.contains(static_cast<std::pmr::string>(gr::tag::END_OF_STREAM)); }));
        }
    };
    policyTest("immediate stop cannot resurrect an async start") = [] {
        gr::MsgPortOut requests;
        gr::Graph      graph;
        auto&          source = graph.emplaceBlock<Source>();
        source.limit          = std::numeric_limits<std::size_t>::max();
        auto& first           = graph.emplaceBlock<Sink>();
        auto& second          = graph.emplaceBlock<Sink>();
        expect(graph.connect<"out", "in">(source, first).has_value());
        expect(graph.connect<"second", "in">(source, second).has_value());
        gr::SchedulerWrapper<EntryGateScheduler<Policy>> scheduler;
        scheduler.setGraph(std::move(graph));
        auto& implementation = scheduler.blockRef();
        expect(requests.connect(*scheduler.asBlockModel()->msgIn).has_value());
        expect(implementation.changeStateTo(gr::lifecycle::State::INITIALISED).has_value());
        // init() has already serviced its initial messages. The asynchronous
        // entry now takes this message before attempting its RUNNING transition.
        gr::sendMessage<gr::message::Command::Set>(requests, implementation.unique_name, "holdAsyncEntry", gr::property_map{});
        expect(scheduler.start().has_value());
        expect(wait([&] { return implementation.entryHeld.load(); }));
        auto stopped = std::async(std::launch::async, [&] { scheduler.stop(); });
        expect(wait([&] { return scheduler.state() == gr::lifecycle::State::STOPPED; }));
        implementation.releaseEntry = true;
        implementation.releaseEntry.notify_all();
        const bool joined = stopped.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        expect(joined) << "the accepted asynchronous start restarted after Stop";
        if (!joined) {
            // An unbounded resurrected run cannot complete on its own. Retire
            // it without competing with the controller's join of the thread.
            std::ignore = implementation.changeStateTo(gr::lifecycle::State::REQUESTED_STOP);
        }
        stopped.get();
        expect(scheduler.state() == gr::lifecycle::State::STOPPED);
        expect(eq(source.starts.load(), 0UZ));
        expect(!source.entered.load());
        expect(eq(source.produced, 0UZ));
        expect(eq(first.consumed, 0UZ));
        expect(eq(second.consumed, 0UZ));
    };
}
int main() {
    qualify<gr::scheduler::ExecutionPolicy::singleThreaded>();
    qualify<gr::scheduler::ExecutionPolicy::multiThreaded>();
}
