#include <boost/ut.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <expected>
#include <format>
#include <memory_resource>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/CircularBuffer.hpp>
#include <gnuradio-4.0/ComputeDomain.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
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

// stdout redirected into an anonymous file while this lives, so a case can read what the graph reported
struct StdoutCapture {
    std::FILE* _file    = std::tmpfile();
    int        _savedFd = -1;

    StdoutCapture() {
        std::fflush(stdout);
        _savedFd = ::dup(STDOUT_FILENO);
        ::dup2(::fileno(_file), STDOUT_FILENO);
    }

    StdoutCapture(const StdoutCapture&)            = delete;
    StdoutCapture& operator=(const StdoutCapture&) = delete;

    ~StdoutCapture() {
        std::fflush(stdout);
        ::dup2(_savedFd, STDOUT_FILENO);
        ::close(_savedFd);
        std::fclose(_file);
    }

    // pread leaves the offset alone, which stdout shares through dup2
    [[nodiscard]] std::string text() const {
        std::fflush(stdout);
        std::string           captured;
        std::array<char, 512> chunk{};
        for (ssize_t nRead = ::pread(::fileno(_file), chunk.data(), chunk.size(), 0); nRead > 0; nRead = ::pread(::fileno(_file), chunk.data(), chunk.size(), static_cast<off_t>(captured.size()))) {
            captured.append(chunk.data(), static_cast<std::size_t>(nRead));
        }
        return captured;
    }
};

// formats a port as the edge formatter prints it; the '/' keeps one block name from matching inside a longer one
[[nodiscard]] std::string portOf(std::string_view blockName, std::string_view port) { return std::format("{}/{}", blockName, port); }

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

[[nodiscard]] std::optional<gr::property_map> awaitReplyData(gr::MsgPortIn& port, std::string_view endpoint) {
    for (std::size_t i = 0UZ; i < 3000UZ; ++i) {
        auto messages = port.streamReader().get();
        for (const gr::Message& message : messages) {
            if (message.endpoint == endpoint && message.data.has_value()) {
                return message.data.value();
            }
        }
        std::ignore = messages.consume(messages.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
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

// the unique names of the graph's blocks in insertion order, comma separated
[[nodiscard]] std::string blockNames(const gr::Graph& graph) {
    std::string names;
    for (const std::shared_ptr<gr::BlockModel>& block : graph.blocks()) {
        names += std::format("{}{}", names.empty() ? "" : ", ", block->uniqueName());
    }
    return names;
}

// the message of the gr::exception that emplace throws, empty when it returns
[[nodiscard]] std::string refusal(auto emplace) {
    try {
        emplace();
    } catch (const gr::exception& e) {
        return e.message;
    }
    return {};
}

struct CountingResource : std::pmr::memory_resource {
    std::size_t nAllocations = 0UZ;

    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++nAllocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override { std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment); }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
};

// a compute-domain provider whose resource outlives every buffer bound to it
std::pmr::memory_resource* domainResource(const gr::ComputeDomain&, void*) {
    static CountingResource resource;
    return &resource;
}

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

    "a block emplaced by name with a setting it does not declare stays out of the graph"_test = [] {
        qa_edit::registerTestBlocks();
        const std::string tunableType{gr::meta::type_name<qa_edit::Tunable>()};

        gr::Graph flow;
        std::ignore                     = flow.emplaceBlock<qa_edit::Source>();
        const std::size_t nBlocksBefore = flow.blocks().size();
        const std::string namesBefore   = qa_edit::blockNames(flow);

        const std::string refused = qa_edit::refusal([&] { std::ignore = flow.emplaceBlock(tunableType, {{"gian", 2.0f}}); });
        expect(refused.starts_with("settings could not be applied") && refused.contains("gian")) << "the refusal must reach the caller as thrown, got: " << refused;
        expect(eq(flow.blocks().size(), nBlocksBefore)) << "the block stayed in the graph after throwing: " << refused;
        expect(eq(qa_edit::blockNames(flow), namesBefore)) << "the graph holds other blocks after throwing: " << refused;

        const std::shared_ptr<gr::BlockModel>& accepted = flow.emplaceBlock(tunableType, {{"gain", 2.0f}});
        expect(eq(flow.blocks().size(), nBlocksBefore + 1UZ)) << "a block with settings it declares must join the graph";
        const std::optional<gr::pmt::Value> gain = accepted->settings().get("gain");
        expect(fatal(gain.has_value())) << "the accepted block reports no gain setting";
        expect(eq(gain->value_or(0.0f), 2.0f)) << "the accepted block must apply its settings";
    };

    "a block emplaced by name with a value its setting refuses stays out of the graph"_test = [] {
        qa_edit::registerTestBlocks();
        const std::string tunableType{gr::meta::type_name<qa_edit::Tunable>()};

        gr::Graph flow;
        std::ignore                     = flow.emplaceBlock<qa_edit::Source>();
        const std::size_t nBlocksBefore = flow.blocks().size();
        const std::string namesBefore   = qa_edit::blockNames(flow);

        const std::string refused = qa_edit::refusal([&] { std::ignore = flow.emplaceBlock(tunableType, {{"gain", std::string("loud")}}); });
        expect(refused.contains("'gain'")) << "the refusal must reach the caller as thrown, got: " << refused;
        expect(eq(flow.blocks().size(), nBlocksBefore)) << "the block stayed in the graph after throwing: " << refused;
        expect(eq(qa_edit::blockNames(flow), namesBefore)) << "the graph holds other blocks after throwing: " << refused;
    };
#endif

    "a typed emplacement with a setting the block does not declare leaves the graph as it was"_test = [] {
        gr::Graph flow;
        std::ignore                     = flow.emplaceBlock<qa_edit::Source>();
        const std::size_t nBlocksBefore = flow.blocks().size();
        const std::string namesBefore   = qa_edit::blockNames(flow);

        const std::string refused = qa_edit::refusal([&] { std::ignore = flow.emplaceBlock<qa_edit::Tunable>({{"gian", 2.0f}}); });
        expect(refused.starts_with("settings could not be applied") && refused.contains("gian")) << "the refusal must reach the caller as thrown, got: " << refused;
        expect(eq(flow.blocks().size(), nBlocksBefore)) << "the block stayed in the graph after throwing: " << refused;
        expect(eq(qa_edit::blockNames(flow), namesBefore)) << "the graph holds other blocks after throwing: " << refused;

        auto& accepted = flow.emplaceBlock<qa_edit::Tunable>({{"gain", 2.0f}});
        expect(eq(flow.blocks().size(), nBlocksBefore + 1UZ)) << "a block with settings it declares must join the graph";
        expect(eq(accepted.gain.value, 2.0f)) << "the accepted block must apply its settings";
        expect(accepted.state() == INITIALISED) << "the accepted block must be initialized";
    };

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

    "an edge naming no memory resource gets the buffer's own default"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::Source>();
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        expect(fatal(eq(flow.edges().size(), 1UZ)));
        expect(flow.connectPendingEdges());

        const gr::Edge& edge = flow.edges()[0];
        expect(edge._dataResource == nullptr) << "an edge nobody gave a resource must leave the choice to the buffer";
        expect(edge._tagResource == nullptr) << "an edge nobody gave a resource must leave the choice to the buffer";
        expect(!source.out.buffer().tagBuffer.isMmapAllocated()) << "a tag is not trivially copyable, so its ring stays on the heap";
        if constexpr (gr::has_posix_mmap_interface) {
            expect(source.out.buffer().streamBuffer.isMmapAllocated()) << "a trivially copyable sample on a platform with mmap must get the double-mapped ring";
        }
    };

    "an edge naming the default resource gets a heap buffer"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::Source>();
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sink, gr::EdgeParameters{.dataResource = std::pmr::get_default_resource(), .tagResource = std::pmr::get_default_resource()}).has_value());
        expect(flow.connectPendingEdges());

        expect(flow.edges()[0]._dataResource == std::pmr::get_default_resource()) << "a named resource must stay as named";
        expect(source.out.dataResource() == std::pmr::get_default_resource()) << "the port's buffer must come from the named resource";
        expect(!source.out.buffer().streamBuffer.isMmapAllocated()) << "a named heap resource must not be replaced by the double-mapped ring";
    };

#ifndef GR_TEST_WITHOUT_BLOCK_REGISTRY // a graph file resolves its block ids through the registry
    "an edge whose graph file names a memory resource keeps it"_test = [] {
        qa_edit::registerTestBlocks();
        gr::ComputeRegistry::instance().register_provider("qa-edit-graph-file", &qa_edit::domainResource);
        auto&             resource        = static_cast<qa_edit::CountingResource&>(*qa_edit::domainResource({}, nullptr));
        const std::size_t nAllocationsOld = resource.nAllocations;

        const std::string graphFile = std::format("blocks:\n"
                                                  "  - id: {}\n"
                                                  "    parameters:\n"
                                                  "      name: source\n"
                                                  "      compute_domain: \"gpu:qa-edit-graph-file\"\n"
                                                  "  - id: {}\n"
                                                  "    parameters:\n"
                                                  "      name: sink\n"
                                                  "connections:\n"
                                                  "  - [source, out, sink, in]\n",
            gr::meta::type_name<qa_edit::Source>(), gr::meta::type_name<qa_edit::Sink>());
        auto              flow      = gr::loadGrc(gr::globalPluginLoader(), graphFile);
        expect(fatal(eq(flow->edges().size(), 1UZ)));
        expect(flow->connectPendingEdges());

        const gr::Edge& edge = flow->edges()[0];
        expect(edge._dataResource == &resource) << "the resource the graph file's compute domain names must reach the edge";
        expect(edge._tagResource == &resource) << "the resource the graph file's compute domain names must reach the edge";
        auto& source = *static_cast<qa_edit::Source*>(edge._sourceBlock->raw());
        expect(source.out.dataResource() == &resource) << "the port's buffer must come from the named resource";
        expect(gt(resource.nAllocations, nAllocationsOld)) << "the named resource must have served the buffer";
    };
#endif

    "an edge naming only its tag resource keeps it while the compute domain fills the data resource"_test = [] {
        gr::ComputeRegistry::instance().register_provider("qa-edit-mixed", &qa_edit::domainResource);
        auto&                     domainPool = static_cast<qa_edit::CountingResource&>(*qa_edit::domainResource({}, nullptr));
        qa_edit::CountingResource tagPool;
        gr::Graph                 flow;
        auto&                     source = flow.emplaceBlock<qa_edit::Source>({{"compute_domain", std::string("gpu:qa-edit-mixed")}});
        auto&                     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sink, gr::EdgeParameters{.tagResource = &tagPool}).has_value());
        expect(flow.connectPendingEdges());

        const gr::Edge& edge = flow.edges()[0];
        expect(edge._tagResource == &tagPool) << "a tag resource the caller named must stay as named";
        expect(source.out.tagResource() == &tagPool) << "the port's tag ring must come from the named resource";
        expect(edge._dataResource == &domainPool) << "the compute domain must fill the data resource nobody named";
        expect(source.out.dataResource() == &domainPool) << "the port's stream ring must come from the compute domain's resource";
    };

    "an edge naming only its data resource keeps it while the compute domain fills the tag resource"_test = [] {
        gr::ComputeRegistry::instance().register_provider("qa-edit-mixed", &qa_edit::domainResource);
        auto&                     domainPool = static_cast<qa_edit::CountingResource&>(*qa_edit::domainResource({}, nullptr));
        qa_edit::CountingResource dataPool;
        gr::Graph                 flow;
        auto&                     source = flow.emplaceBlock<qa_edit::Source>({{"compute_domain", std::string("gpu:qa-edit-mixed")}});
        auto&                     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sink, gr::EdgeParameters{.dataResource = &dataPool}).has_value());
        expect(flow.connectPendingEdges());

        const gr::Edge& edge = flow.edges()[0];
        expect(edge._dataResource == &dataPool) << "a data resource the caller named must stay as named";
        expect(source.out.dataResource() == &dataPool) << "the port's stream ring must come from the named resource";
        expect(edge._tagResource == &domainPool) << "the compute domain must fill the tag resource nobody named";
        expect(source.out.tagResource() == &domainPool) << "the port's tag ring must come from the compute domain's resource";
    };

    "an edge naming both resources keeps both under a compute domain"_test = [] {
        gr::ComputeRegistry::instance().register_provider("qa-edit-mixed", &qa_edit::domainResource);
        qa_edit::CountingResource dataPool;
        qa_edit::CountingResource tagPool;
        gr::Graph                 flow;
        auto&                     source = flow.emplaceBlock<qa_edit::Source>({{"compute_domain", std::string("gpu:qa-edit-mixed")}});
        auto&                     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, sink, gr::EdgeParameters{.dataResource = &dataPool, .tagResource = &tagPool}).has_value());
        expect(flow.connectPendingEdges());

        const gr::Edge& edge = flow.edges()[0];
        expect(edge._dataResource == &dataPool) << "a data resource the caller named must stay as named";
        expect(edge._tagResource == &tagPool) << "a tag resource the caller named must stay as named";
        expect(source.out.dataResource() == &dataPool) << "the port's stream ring must come from the named resource";
        expect(source.out.tagResource() == &tagPool) << "the port's tag ring must come from the named resource";
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

    // the same shape on the subgraph export path, and reached without a scheduler because the
    // handler answers the message the wrapper processes
    "a subgraph export message whose exportFlag is of the wrong type is refused"_test = [] {
        gr::GraphWrapper<gr::Graph> subgraph;

        gr::MsgPortOut toSubgraph;
        gr::MsgPortIn  fromSubgraph;
        expect(toSubgraph.connect(*subgraph.msgIn).has_value());
        expect(subgraph.msgOut->connect(fromSubgraph).has_value());

        qa_edit::sendMessage(toSubgraph, gr::graph::property::kSubgraphExportPort,
            {{"uniqueBlockName", std::string("inner")}, {"portDirection", std::string("output")}, //
                {"portName", std::string("out")}, {"exportFlag", std::string("yes")}});

        subgraph.processScheduledMessages();

        bool refused = false;
        auto replies = fromSubgraph.streamReader().get();
        for (const gr::Message& reply : replies) {
            refused = refused || !reply.data.has_value();
        }
        std::ignore = replies.consume(replies.size());
        expect(refused) << "an exportFlag that is not a bool must be reported, not end the process";
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

    "a second edge into a taken stream input replaces the first"_test = [] {
        using namespace gr::serialization_fields;

        gr::Graph flow;
        auto&     first  = flow.emplaceBlock<qa_edit::Source>({{"name", std::string("first")}});
        auto&     second = flow.emplaceBlock<qa_edit::Source>({{"name", std::string("second")}});
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(first, sink).has_value());

        bool        connected = false;
        std::string reported;
        {
            qa_edit::StdoutCapture capture;
            // the typed connect names the input by index and this one names it by name
            connected = flow.connect(second, gr::PortDefinition("out"), sink, gr::PortDefinition("in")).has_value();
            reported  = capture.text();
        }
        expect(connected);

        expect(fatal(eq(flow.edges().size(), 1UZ))) << "the displaced edge stayed listed";
        expect(eq(flow.edges()[0].sourceBlock()->uniqueName(), std::string_view(second.unique_name))) << "the earlier edge was kept instead of the later one";
        expect(reported.contains(qa_edit::portOf("first", "0")) && reported.contains(qa_edit::portOf("second", "out"))) << "the replacement must name both edges, reported: " << reported;

        expect(flow.connectPendingEdges());
        expect(flow.edges()[0].state() == gr::Edge::EdgeState::Connected);
        expect(eq(first.out.nReaders(), 0UZ)) << "the displaced source still feeds the input";
        expect(eq(second.out.nReaders(), 1UZ)) << "the listed edge carries no data";

        const std::optional<gr::Message> inspected = flow.propertyCallbackGraphInspect(gr::graph::property::kGraphInspect, {});
        expect(fatal(inspected.has_value() && inspected->data.has_value()));
        const auto* inspectedEdges = inspected->data->at(std::pmr::string(BLOCK_EDGES)).get_if<gr::property_map>();
        expect(fatal(inspectedEdges != nullptr));
        expect(fatal(eq(inspectedEdges->size(), 1UZ))) << "the inspect reply lists an edge the graph does not carry";
        const auto* inspectedEdge = inspectedEdges->begin()->second.get_if<gr::property_map>();
        expect(fatal(inspectedEdge != nullptr));
        expect(eq(inspectedEdge->at(std::pmr::string(EDGE_SOURCE_BLOCK)).value_or(std::string_view{}), std::string_view(second.unique_name))) << "the inspect reply names the displaced source";
    };

    "an edge emplaced into a taken stream input replaces the listed edge"_test = [] {
        using namespace gr::serialization_fields;

        qa_edit::TestScheduler scheduler;
        gr::Graph              flow;
        auto&                  first  = flow.emplaceBlock<qa_edit::Source>({{"name", std::string("first")}});
        auto&                  second = flow.emplaceBlock<qa_edit::Source>({{"name", std::string("second")}});
        auto&                  sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(first, sink).has_value());
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        std::optional<gr::property_map> reply;
        std::string                     reported;
        {
            qa_edit::StdoutCapture capture;
            qa_edit::sendMessage(toScheduler, gr::scheduler::property::kEmplaceEdge,
                {{std::pmr::string(EDGE_SOURCE_BLOCK), std::string(second.unique_name)}, {std::pmr::string(EDGE_SOURCE_PORT), std::string("out")},           //
                    {std::pmr::string(EDGE_DESTINATION_BLOCK), std::string(sink.unique_name)}, {std::pmr::string(EDGE_DESTINATION_PORT), std::string("in")}, //
                    {std::pmr::string(EDGE_MIN_BUFFER_SIZE), gr::undefined_Size}, {std::pmr::string(EDGE_WEIGHT), std::int32_t{0}},                          //
                    {std::pmr::string(EDGE_NAME), std::string("replacement")}});
            reply    = qa_edit::awaitReplyData(fromScheduler, gr::scheduler::property::kEdgeEmplaced);
            reported = capture.text();
        }
        expect(fatal(reply.has_value())) << "the edge was never emplaced";

        const std::span<const gr::Edge> edges = scheduler.graph().edges();
        expect(fatal(eq(edges.size(), 1UZ))) << "the displaced edge stayed listed";
        expect(eq(edges[0].sourceBlock()->uniqueName(), std::string_view(second.unique_name))) << "the earlier edge was kept instead of the emplaced one";
        expect(reported.contains(qa_edit::portOf("first", "0")) && reported.contains(qa_edit::portOf("second", "out"))) << "the replacement must name both edges, reported: " << reported;

        const auto* displaced = reply->contains(std::pmr::string("displacedEdges")) ? reply->at(std::pmr::string("displacedEdges")).get_if<gr::property_map>() : nullptr;
        expect(fatal(displaced != nullptr)) << "the reply does not list the displaced edges";
        expect(fatal(eq(displaced->size(), 1UZ))) << "the reply lists a different number of displaced edges";
        const auto* displacedEdge = displaced->begin()->second.get_if<gr::property_map>();
        expect(fatal(displacedEdge != nullptr));
        expect(eq(displacedEdge->at(std::pmr::string(EDGE_SOURCE_BLOCK)).value_or(std::string_view{}), std::string_view(first.unique_name))) << "the reply names another edge as displaced";
        expect(eq(displacedEdge->at(std::pmr::string(EDGE_DESTINATION_BLOCK)).value_or(std::string_view{}), std::string_view(sink.unique_name)));
        expect(eq(first.out.nReaders(), 0UZ)) << "the displaced source still feeds the input";
        expect(eq(second.out.nReaders(), 1UZ)) << "the emplaced source does not feed the input";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        for (std::size_t i = 0UZ; i < 3000UZ && scheduler.state() != STOPPED; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(scheduler.state() == STOPPED);
    };

    // the functions the RemoveEdge and EmplaceEdge handlers call, in the order a rewire takes
    "an input freed by removing its edge takes a new edge without a replacement"_test = [] {
        gr::Graph flow;
        auto&     first  = flow.emplaceBlock<qa_edit::Source>({{"name", std::string("first")}});
        auto&     second = flow.emplaceBlock<qa_edit::Source>({{"name", std::string("second")}});
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(first, sink).has_value());
        expect(flow.connectPendingEdges());

        const auto removed = flow.removeEdgeBySourcePort(first.unique_name, "out", sink.unique_name, "in");
        expect(fatal(removed.has_value())) << "the edge could not be removed: " << (removed.has_value() ? std::string{} : removed.error().message);

        std::expected<std::vector<gr::Edge>, gr::Error> emplaced;
        std::string                                     reported;
        {
            qa_edit::StdoutCapture capture;
            emplaced = flow.emplaceEdge(second.unique_name, "out", sink.unique_name, "in", gr::undefined_size, 0, "rewired");
            reported = capture.text();
        }
        expect(fatal(emplaced.has_value())) << "the edge could not be emplaced: " << (emplaced.has_value() ? std::string{} : emplaced.error().message);
        expect(emplaced->empty()) << "a free input returned a displaced edge";

        expect(fatal(eq(flow.edges().size(), 1UZ))) << "the rewire left a different number of edges";
        expect(eq(flow.edges()[0].sourceBlock()->uniqueName(), std::string_view(second.unique_name))) << "the rewired edge is not the one listed";
        expect(!reported.contains(qa_edit::portOf("second", "out"))) << "a free input reported a replacement: " << reported;
        expect(eq(first.out.nReaders(), 0UZ)) << "the removed source still feeds the input";
        expect(eq(second.out.nReaders(), 1UZ)) << "the rewired source does not feed the input";
    };
};

int main() { /* tests are statically registered */ }
