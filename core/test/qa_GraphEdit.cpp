#include <boost/ut.hpp>

#include <bit>
#include <chrono>
#include <cstddef>
#include <format>
#include <string>
#include <thread>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Message.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

namespace qa_edit {

struct Tunable : gr::Block<Tunable> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    gr::Annotated<float, "gain"> gain = 1.0f;

    GR_MAKE_REFLECTABLE(Tunable, in, out, gain);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain; }
};

struct Source : gr::Block<Source> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(Source, out);

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }
};

struct CountingSource : gr::Block<CountingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(CountingSource, out);

    static constexpr std::size_t kSamples = 4096UZ;

    std::size_t _nProduced = 0UZ;

    float processOne() {
        if (++_nProduced >= kSamples) {
            this->requestStop();
        }
        return 1.0f;
    }
};

struct Sink : gr::Block<Sink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(Sink, in);

    std::size_t _nReceived = 0UZ;

    void processOne(float) { _nReceived++; }
};

// one connected and one deliberately unconnected optional output
struct DualSource : gr::Block<DualSource> {
    gr::PortOut<float>               out;
    gr::PortOut<float, gr::Optional> monitor;

    GR_MAKE_REFLECTABLE(DualSource, out, monitor);

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan, gr::OutputSpanLike auto& monitorSpan) {
        outSpan.publish(0UZ);
        monitorSpan.publish(0UZ);
        return gr::work::Status::DONE;
    }
};

// resolvable only through a test-local registry, never the global one, so a lookup that
// succeeds proves which loader served it
struct LoaderCanary : gr::Block<LoaderCanary> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(LoaderCanary, out);

    [[nodiscard]] constexpr float processOne() const noexcept { return 0.0f; }
};

using TestScheduler = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>;

void registerTestBlocks() {
    static const bool registered = [] {
        std::ignore = gr::globalBlockRegistry().insert<Tunable>();
        std::ignore = gr::globalBlockRegistry().insert<Source>();
        std::ignore = gr::globalBlockRegistry().insert<Sink>();
        return true;
    }();
    std::ignore = registered;
}

[[nodiscard]] bool awaitReply(gr::MsgPortIn& port, std::string_view endpoint) {
    for (std::size_t i = 0UZ; i < 3000UZ; ++i) {
        auto messages = port.streamReader().get();
        for (const gr::Message& message : messages) {
            if (message.endpoint == endpoint) {
                return true;
            }
        }
        std::ignore = messages.consume(messages.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

[[nodiscard]] std::string awaitError(gr::MsgPortIn& port, std::string_view endpoint) {
    for (std::size_t i = 0UZ; i < 3000UZ; ++i) {
        auto messages = port.streamReader().get();
        for (const gr::Message& message : messages) {
            if (message.endpoint == endpoint && !message.data.has_value()) {
                return message.data.error().message;
            }
        }
        std::ignore = messages.consume(messages.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return {};
}

void sendMessage(gr::MsgPortOut& port, std::string_view endpoint, gr::property_map data) { gr::sendMessage<gr::message::Command::Set>(port, "", endpoint, std::move(data)); }

} // namespace qa_edit

const boost::ut::suite<"graph editing"> graphEditTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

#ifndef GR_TEST_WITHOUT_BLOCK_REGISTRY // emplacement by name resolves the type through the registry
    "a block emplaced from yaml applies its serialized settings"_test = [] {
        qa_edit::registerTestBlocks();

        qa_edit::TestScheduler scheduler;
        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_edit::Source>();
            auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());
            expect(scheduler.exchange(std::move(flow)).has_value());
        }

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        const std::string blockYaml = std::format("id: {}\nparameters:\n  gain: !!float32 4.5\n", gr::meta::type_name<qa_edit::Tunable>());
        qa_edit::sendMessage(toScheduler, gr::scheduler::property::kEmplaceBlock, {{"yaml", blockYaml}});

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_edit::awaitReply(fromScheduler, gr::scheduler::property::kBlockEmplaced)) << "the block was never emplaced";

        const auto* emplaced = [&]() -> const gr::BlockModel* {
            for (const auto& block : scheduler.graph().blocks()) {
                if (block->typeName().find("Tunable") != std::string_view::npos) {
                    return block.get();
                }
            }
            return nullptr;
        }();
        expect(fatal(emplaced != nullptr)) << "the emplaced block is not in the graph";

        const auto gain = emplaced->settings().get("gain");
        expect(fatal(gain.has_value())) << "the emplaced block reports no gain setting";
        expect(eq(gain->value_or(0.0f), 4.5f)) << "the emplaced block kept its constructor default instead of the serialized value";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
    };

    "a subgraph emplaced by name inherits its parent's plugin loader"_test = [] {
        gr::BlockRegistry     localRegistry;
        gr::SchedulerRegistry localSchedulers;
        std::ignore = localRegistry.insert<qa_edit::LoaderCanary>();
        gr::PluginLoader localLoader(localRegistry, localSchedulers, {});

        const std::string canaryType{gr::meta::type_name<qa_edit::LoaderCanary>()};
        expect(gr::globalPluginLoader().instantiate(canaryType) == nullptr) << "the canary resolves globally, so this test cannot discriminate the loaders";

        gr::Graph                              flow(localLoader);
        const std::shared_ptr<gr::BlockModel>& wrapped = flow.emplaceBlock("gr::Graph", {{"name", std::string("inner")}});
        expect(fatal(wrapped != nullptr));
        expect(eq(std::string{wrapped->name()}, std::string{"inner"})) << "the emplaced subgraph dropped its settings";

        gr::Graph* inner = wrapped->graph();
        expect(fatal(inner != nullptr));
        expect(inner->_pluginLoader == &localLoader) << "the nested graph bound a loader other than its parent's";
        expect(nothrow([&] { std::ignore = inner->emplaceBlock(canaryType, {}); })) << "a type the parent's loader resolves must resolve inside the subgraph";

        auto& toReplace = flow.emplaceBlock<qa_edit::Source>();
        expect(nothrow([&] { std::ignore = flow.replaceBlock(toReplace.unique_name, canaryType, {}); })) << "replaceBlock must consult the graph's own loader";
    };
#endif

    "an exported output with interior consumers feeds both sides of the boundary"_test = [] {
        gr::Graph flow;
        auto      wrapper   = std::make_shared<gr::GraphWrapper<gr::Graph>>();
        auto&     inner     = *wrapper->graph();
        auto&     producer  = inner.emplaceBlock<qa_edit::CountingSource>();
        auto&     innerSink = inner.emplaceBlock<qa_edit::Sink>();
        expect(inner.connect<"out", "in">(producer, innerSink).has_value());

        const std::shared_ptr<gr::BlockModel>& subgraph = flow.addBlock(wrapper);
        expect(wrapper->exportPort(true, producer.unique_name, gr::PortDirection::OUTPUT, "out", "out").has_value());
        std::ignore = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect(subgraph, gr::PortDefinition{"out"}, flow.blocks()[1], gr::PortDefinition{"in"}).has_value());

        expect(flow.edges()[0].hasSameSourcePort(inner.edges()[0])) << "the exported alias and the interior edge reference the same port and must compare equal";

        // the wiring order a scheduler uses: the top-level graph's edges, then the subgraph's
        expect(flow.connectPendingEdges());
        expect(inner.connectPendingEdges());

        expect(eq(producer.out.nReaders(), 2UZ)) << "the boundary split the fan-out across two buffers, so one consumer starves";
    };

    "an unconnected optional output is sized with its connected sibling"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::DualSource>();
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        expect(flow.connectPendingEdges());
        expect(eq(source.monitor.bufferSize(), source.out.bufferSize())) << "a span request past the default capacity returns empty with nothing signaled";
    };

    "a fan-out mixing typed and dynamic connects feeds every consumer"_test = [] {
        auto runMixedFanOut = [](bool typedFirst) {
            const std::string order = typedFirst ? "typed edge first" : "dynamic edge first";

            gr::Graph flow;
            auto&     source      = flow.emplaceBlock<qa_edit::CountingSource>();
            auto&     typedSink   = flow.emplaceBlock<qa_edit::Sink>();
            auto&     dynamicSink = flow.emplaceBlock<qa_edit::Sink>();

            const auto connectTyped   = [&] { return flow.connect<"out", "in">(source, typedSink).has_value(); };
            const auto connectDynamic = [&] { return flow.connect(source, gr::PortDefinition("out"), dynamicSink, gr::PortDefinition("in")).has_value(); };

            if (typedFirst) {
                expect(connectTyped()) << order << ": the typed edge was not accepted";
                expect(connectDynamic()) << order << ": the dynamic edge was not accepted";
            } else {
                expect(connectDynamic()) << order << ": the dynamic edge was not accepted";
                expect(connectTyped()) << order << ": the typed edge was not accepted";
            }

            gr::scheduler::Simple scheduler;
            expect(fatal(scheduler.exchange(std::move(flow)).has_value()));

            std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });
            for (std::size_t i = 0UZ; i < 2000UZ && (typedSink._nReceived == 0UZ || dynamicSink._nReceived == 0UZ); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::ignore = scheduler.changeStateTo(REQUESTED_STOP);
            runner.join();

            expect(gt(typedSink._nReceived, 0UZ)) << order << ": the index-addressed consumer of the fan-out received nothing";
            expect(gt(dynamicSink._nReceived, 0UZ)) << order << ": the name-addressed consumer of the fan-out received nothing";
        };

        runMixedFanOut(true);
        runMixedFanOut(false);
    };

    "a mixed-style fan-out is one adjacency-list entry"_test = [] {
        gr::Graph flow;
        auto&     source      = flow.emplaceBlock<qa_edit::Source>();
        auto&     typedSink   = flow.emplaceBlock<qa_edit::Sink>();
        auto&     dynamicSink = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, typedSink).has_value());
        expect(flow.connect(source, gr::PortDefinition("out"), dynamicSink, gr::PortDefinition("in")).has_value());

        const gr::graph::AdjacencyList         adjacencyList = gr::graph::computeAdjacencyList(flow);
        const std::shared_ptr<gr::BlockModel>& sourceModel   = flow.blocks().front();

        expect(fatal(adjacencyList.contains(sourceModel)));
        expect(eq(adjacencyList.at(sourceModel).size(), 1UZ)) << "one output port must not occupy two entries";
        expect(eq(gr::graph::outgoingEdges(adjacencyList, sourceModel, gr::PortDefinition("out")).size(), 2UZ)) << "the name-addressed query missed an edge";
        expect(eq(gr::graph::outgoingEdges(adjacencyList, sourceModel, gr::PortDefinition(0UZ)).size(), 2UZ)) << "the index-addressed query missed an edge";
    };

    "removing one edge of a fan-out leaves the sibling flowing and stays removed"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::Source>();
        auto&     sinkA  = flow.emplaceBlock<qa_edit::Sink>();
        auto&     sinkB  = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sinkA).has_value());
        expect(flow.connect<"out", "in">(source, sinkB).has_value());
        expect(eq(flow.edges().size(), 2UZ));

        expect(flow.connectPendingEdges()) << "the fan-out did not connect";

        const auto removed = flow.removeEdgeBySourcePort(source.unique_name, "out", sinkA.unique_name, "in");
        expect(fatal(removed.has_value())) << "the edge could not be removed: " << (removed.has_value() ? std::string{} : removed.error().message);
        expect(eq(*removed, 1UZ)) << "removing one edge of the fan-out removed a different number of edges";
        expect(eq(flow.edges().size(), 1UZ)) << "the removed edge was left in the edge list and will be resurrected on restart";
        expect(eq(flow.edges()[0].destinationBlock()->uniqueName(), std::string_view(sinkB.unique_name))) << "the wrong edge was removed";
        expect(flow.edges()[0].state() == gr::Edge::EdgeState::Connected) << "the sibling edge was left dead after the port teardown";

        // a restart must not bring the removed edge back
        flow.disconnectAllEdges();
        expect(flow.connectPendingEdges()) << "the graph did not reconnect after a restart";
        expect(eq(flow.edges().size(), 1UZ)) << "the removed edge came back on restart";

        expect(!flow.removeEdgeBySourcePort(source.unique_name, "out", sinkA.unique_name, "in").has_value()) << "removing an absent edge reported success";
    };

#ifndef GR_TEST_WITHOUT_BLOCK_REGISTRY // emplacement by name resolves the type through the registry
    "emplacing and removing blocks while the graph runs"_test = [] {
        constexpr std::size_t kCycles = 8UZ;

        qa_edit::registerTestBlocks();

        qa_edit::TestScheduler scheduler;
        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_edit::Source>();
            auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());
            expect(scheduler.exchange(std::move(flow)).has_value());
        }

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        const std::string blockYaml = std::format("id: {}\nparameters:\n  gain: !!float32 2.0\n", gr::meta::type_name<qa_edit::Tunable>());
        for (std::size_t cycle = 0UZ; cycle < kCycles; ++cycle) {
            qa_edit::sendMessage(toScheduler, gr::scheduler::property::kEmplaceBlock, {{"yaml", blockYaml}});
            expect(qa_edit::awaitReply(fromScheduler, gr::scheduler::property::kBlockEmplaced)) << "cycle " << cycle << ": block was never emplaced";

            std::string emplacedName;
            for (const auto& block : scheduler.graph().blocks()) {
                if (block->typeName().find("Tunable") != std::string_view::npos) {
                    emplacedName = block->uniqueName();
                }
            }
            expect(!emplacedName.empty()) << "cycle " << cycle << ": the emplaced block is not in the graph";

            qa_edit::sendMessage(toScheduler, gr::scheduler::property::kRemoveBlock, {{"uniqueName", emplacedName}});
            expect(qa_edit::awaitReply(fromScheduler, gr::scheduler::property::kBlockRemoved)) << "cycle " << cycle << ": block was never removed";
        }

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        for (std::size_t i = 0UZ; i < 3000UZ && scheduler.state() != STOPPED; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(scheduler.state() == STOPPED) << "the scheduler did not stop after the edit cycles";
    };

#endif

    // the fields of an edge message come from whoever sent it, so a value of the wrong type is
    // unusable input: the message is answered as incomplete rather than ending the process, which
    // is what a terminating pointer read of the buffer size or the weight would do
    "an edge message whose weight is of the wrong type is refused"_test = [] {
        using namespace gr::serialization_fields;

        qa_edit::TestScheduler scheduler;
        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_edit::Source>();
            auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());
            expect(scheduler.exchange(std::move(flow)).has_value());
        }

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        qa_edit::sendMessage(toScheduler, gr::scheduler::property::kEmplaceEdge,
            {{std::pmr::string(EDGE_SOURCE_BLOCK), std::string("source")}, {std::pmr::string(EDGE_SOURCE_PORT), std::string("out")},           //
                {std::pmr::string(EDGE_DESTINATION_BLOCK), std::string("sink")}, {std::pmr::string(EDGE_DESTINATION_PORT), std::string("in")}, //
                {std::pmr::string(EDGE_MIN_BUFFER_SIZE), gr::undefined_Size}, {std::pmr::string(EDGE_WEIGHT), std::string("heavy")},           //
                {std::pmr::string(EDGE_NAME), std::string("wrong weight")}});

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        const std::string reported = qa_edit::awaitError(fromScheduler, gr::scheduler::property::kEmplaceEdge);
        expect(!reported.empty()) << "a weight of the wrong type must be reported, not end the process";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
    };

    "an edge sized by duration follows the sample rate"_test = [] {
        using gr::graph::edgeBufferSizeFor;
        using gr::graph::kDefaultEdgeBufferSeconds;
        using gr::graph::kMaxEdgeBufferSize;
        using gr::graph::kMinEdgeBufferSize;

        constexpr double rates[] = {48.0e3, 2.4e6, 25.0e6, 61.44e6};
        for (const double rate : rates) {
            const std::size_t nSamples = edgeBufferSizeFor(rate);
            expect(eq(nSamples, std::bit_ceil(nSamples))) << rate << ": the ring is not a power of two";
            expect(ge(nSamples, kMinEdgeBufferSize)) << rate << ": the ring is below the floor";
            expect(le(nSamples, kMaxEdgeBufferSize)) << rate << ": the ring is above the ceiling";
            if (nSamples > kMinEdgeBufferSize && nSamples < kMaxEdgeBufferSize) {
                expect(ge(static_cast<double>(nSamples) / rate, kDefaultEdgeBufferSeconds)) << rate << ": the ring holds less than the stated duration";
                expect(lt(static_cast<double>(nSamples) / rate, 2.0 * kDefaultEdgeBufferSeconds)) << rate << ": rounding is the only excess allowed";
            }
        }

        expect(gt(edgeBufferSizeFor(25.0e6), edgeBufferSizeFor(2.4e6))) << "ten times the rate must not give the same ring, which is what a fixed count does";
        expect(eq(edgeBufferSizeFor(48.0e3), kMinEdgeBufferSize)) << "a rate too low to fill the smallest ring must get the smallest ring";
        expect(eq(edgeBufferSizeFor(61.44e6), kMaxEdgeBufferSize)) << "a rate asking for more than the ceiling must be held at it";
        expect(eq(edgeBufferSizeFor(0.0), kMinEdgeBufferSize)) << "an unknown rate must get the smallest ring, not an empty one";
        expect(eq(edgeBufferSizeFor(-1.0), kMinEdgeBufferSize)) << "a negative rate must get the smallest ring, not an empty one";
        expect(eq(edgeBufferSizeFor(1.0e12), kMaxEdgeBufferSize)) << "a rate asking for hundreds of megabytes must be held at the ceiling";
        expect(eq(edgeBufferSizeFor(2.4e6, 0.001), kMinEdgeBufferSize)) << "a shorter duration must reach the floor at a rate the default does not";
    };
};

int main() { /* tests are statically registered */ }
