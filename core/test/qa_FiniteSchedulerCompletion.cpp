#include <atomic>
#include <boost/ut.hpp>
#include <chrono>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <thread>
namespace {
struct Source : gr::Block<Source> {
    gr::PortIn<int, gr::Optional> clock;
    gr::PortOut<int>              out, second;
    GR_MAKE_REFLECTABLE(Source, clock, out, second);
    std::size_t       limit = 20, produced = 0;
    std::atomic<bool> entered = false;
    void              start() { produced = 0; }
    gr::work::Status  processBulk(gr::InputSpanLike auto&, gr::OutputSpanLike auto& a, gr::OutputSpanLike auto& b) {
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
    std::size_t       consumed = 0;
    bool              stall    = false;
    std::atomic<bool> entered  = false;
    gr::work::Status  processBulk(gr::InputSpanLike auto& input) {
        entered = true;
        if (stall) {
            std::ignore = input.consume(0);
            return gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        consumed += input.size();
        std::ignore = input.consume(input.size());
        return gr::work::Status::OK;
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
