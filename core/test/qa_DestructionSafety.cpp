#include <boost/ut.hpp>

#include <chrono>
#include <memory>
#include <thread>

#include <gnuradio-4.0/BlockingSync.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>

namespace qa_destroy {

struct Source : gr::Block<Source> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(Source, out);

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }
};

struct Sink : gr::Block<Sink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(Sink, in);

    void processOne(float) {}
};

struct TimedSource : gr::Block<TimedSource>, gr::BlockingSync<TimedSource> {
    gr::PortOut<float> out;

    gr::Annotated<float, "sample rate">     sample_rate = 100000.0f;
    gr::Annotated<gr::Size_t, "chunk size"> chunk_size  = 16U;

    GR_MAKE_REFLECTABLE(TimedSource, out, sample_rate, chunk_size);

    ~TimedSource() {
        // leave the active states before ~Block() reaches back into this (already-destroyed) derived class
        std::ignore = this->changeStateTo(gr::lifecycle::State::REQUESTED_STOP);
        this->stopTimerAndJoin();
    }

    void start() { this->blockingSyncStart(); }
    void stop() { this->blockingSyncStop(); }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const std::size_t nSamples = this->syncSamples(outSpan);
        for (std::size_t i = 0UZ; i < nSamples; ++i) {
            outSpan[i] = 1.0f;
        }
        outSpan.publish(nSamples);
        return gr::work::Status::OK;
    }
};

using InnerScheduler = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>;

[[nodiscard]] gr::Graph makeGraph() {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<Source>();
    auto&     sink   = flow.emplaceBlock<Sink>();
    expect(flow.connect<"out", "in">(source, sink).has_value());
    return flow;
}

} // namespace qa_destroy

const boost::ut::suite<"destruction safety"> destructionSafetyTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "a started SchedulerWrapper is destructible without a stop"_test = [] {
        constexpr std::size_t kCycles = 10UZ;

        for (std::size_t cycle = 0UZ; cycle < kCycles; ++cycle) {
            gr::SchedulerWrapper<qa_destroy::InnerScheduler> wrapper;
            wrapper.setGraph(qa_destroy::makeGraph());
            wrapper.start();
            // no stop(): the destructor must request it and join the scheduler thread
        }
        expect(true) << "construct/start/destroy cycles completed";
    };

    "a SchedulerWrapper may be started twice"_test = [] {
        gr::SchedulerWrapper<qa_destroy::InnerScheduler> wrapper;
        wrapper.setGraph(qa_destroy::makeGraph());

        wrapper.start();
        wrapper.stop();
        wrapper.start(); // must not assign over a joinable thread
        wrapper.stop();

        expect(true) << "start/stop/start/stop completed";
    };

    "a BlockingSync block is destructible while its timer runs"_test = [] {
        constexpr std::size_t kCycles = 10UZ;

        for (std::size_t cycle = 0UZ; cycle < kCycles; ++cycle) {
            auto block = std::make_unique<qa_destroy::TimedSource>();
            block->init(std::make_shared<gr::Sequence>());
            expect(block->changeStateTo(RUNNING).has_value());
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            block.reset(); // ~TimedSource() must stop and join the timer before its members go
        }
        expect(true) << "construct/start/destroy cycles completed";
    };
};

int main() { /* tests are statically registered */ }
