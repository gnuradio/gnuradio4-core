#include <atomic>
#include <boost/ut.hpp>
#include <chrono>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <limits>
#include <thread>
#include <type_traits>
namespace {
struct Source : gr::Block<Source> {
    gr::PortIn<int, gr::Optional> clock;
    gr::PortOut<int>              out, second;
    GR_MAKE_REFLECTABLE(Source, clock, out, second);
    std::size_t       limit = 20, produced = 0;
    std::atomic<bool> entered     = false;
    bool              failOnStart = false;
    void              start() {
        if (failOnStart) {
            throw gr::exception("source refused to start");
        }
        produced = 0;
    }
    gr::work::Status processBulk(gr::InputSpanLike auto&, gr::OutputSpanLike auto& a, gr::OutputSpanLike auto& b) {
        entered = true;
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
    std::size_t       consumed   = 0;
    bool              stall      = false;
    bool              failOnWork = false;
    std::size_t       maxConsume = std::numeric_limits<std::size_t>::max();
    std::atomic<bool> entered    = false;
    gr::work::Status  processBulk(gr::InputSpanLike auto& input) {
        entered = true;
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
} // namespace
template<gr::scheduler::ExecutionPolicy Policy>
void qualify() {
    using namespace boost::ut;
    "async finite completion settles state and drains both output ports"_test = [] {
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
    "drain stop cancels sources and consumes accepted data; stalled drain times out"_test = [] {
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
            expect(eq(scheduler.stopAfterDrain(std::chrono::milliseconds(50)), !stall));
            if (!stall) {
                expect(eq(src.produced, a.consumed));
                expect(eq(src.produced, b.consumed));
            }
            expect(scheduler.state() == gr::lifecycle::State::STOPPED);
        }
    };
    "drain stop quiesces a nested scheduler before cancelling its source"_test = [] {
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
    "an asynchronous run error is not also a start failure"_test = [] {
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
    "a refused asynchronous start keeps its startup error"_test = [] {
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
    "drain waits for slow consumers on externally driven wrapped and bare runs"_test = [] {
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
    "drain retries while RUNNING is published but sources have not started"_test = [] {
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
    "plain stop does not publish drain EOS and its graph restarts"_test = [] {
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
            expect(std::ranges::none_of(tags, [](const auto& tag) { return tag.map.contains(static_cast<std::pmr::string>(gr::tag::END_OF_STREAM)); }));
        }
    };
    "immediate stop cannot resurrect an async start"_test = [] {
        for (int i = 0; i < 20; ++i) {
            gr::Graph graph;
            auto&     src = graph.emplaceBlock<Source>();
            auto&     a   = graph.emplaceBlock<Sink>();
            auto&     b   = graph.emplaceBlock<Sink>();
            expect(graph.connect<"out", "in">(src, a).has_value());
            expect(graph.connect<"second", "in">(src, b).has_value());
            gr::SchedulerWrapper<gr::scheduler::Simple<Policy>> scheduler;
            scheduler.setGraph(std::move(graph));
            expect(scheduler.start().has_value());
            scheduler.stop();
            expect(scheduler.state() == gr::lifecycle::State::STOPPED);
        }
    };
}
int main() {
    qualify<gr::scheduler::ExecutionPolicy::singleThreaded>();
    qualify<gr::scheduler::ExecutionPolicy::multiThreaded>();
}
