#include <boost/ut.hpp>

#include <array>
#include <bit>
#include <memory_resource>
#include <new>
#include <vector>
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

template<typename T, std::size_t N = 2UZ>
struct ReplacementPorts : gr::Block<ReplacementPorts<T, N>> {
    std::array<gr::PortIn<T>, N> in;
    std::array<gr::PortOut<T>, N> out;
    GR_MAKE_REFLECTABLE(ReplacementPorts, in, out);
    gr::work::Status processBulk(auto&, auto&) { return gr::work::Status::OK; }
};

struct FailingOptionalPort : gr::PortOut<float, gr::Optional> {
    std::expected<void, gr::Error> resizeBuffer(std::size_t, std::pmr::memory_resource* = nullptr, std::pmr::memory_resource* = nullptr) {
        return std::unexpected(gr::Error("injected optional-output allocation failure"));
    }
};
struct FailingReplacement : gr::Block<FailingReplacement> {
    std::array<gr::PortIn<float>, 2> in;
    std::array<gr::PortOut<float>, 2> out;
    FailingOptionalPort monitor;
    GR_MAKE_REFLECTABLE(FailingReplacement, in, out, monitor);
    gr::work::Status processBulk(auto&, auto&, auto&) { return gr::work::Status::OK; }
};

struct ReplacementMonitor : gr::Block<ReplacementMonitor> {
    gr::PortIn<float> in;
    gr::PortOut<float> out;
    gr::PortOut<float, gr::Optional> monitor;
    GR_MAKE_REFLECTABLE(ReplacementMonitor, in, out, monitor);
    gr::work::Status processBulk(auto&, auto&, auto&) { return gr::work::Status::OK; }
};

struct PairedInputs : gr::Block<PairedInputs> {
    gr::PortIn<float> first, second;
    GR_MAKE_REFLECTABLE(PairedInputs, first, second);
    gr::work::Status processBulk(std::span<const float>, std::span<const float>) { return gr::work::Status::OK; }
};
struct ReorderedInputs : gr::Block<ReorderedInputs> {
    gr::PortIn<float> second, first;
    GR_MAKE_REFLECTABLE(ReorderedInputs, second, first);
    gr::work::Status processBulk(std::span<const float>, std::span<const float>) { return gr::work::Status::OK; }
};

struct FailingResource : std::pmr::memory_resource {
    bool fail = false;
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (fail) { throw std::bad_alloc(); }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override { std::pmr::new_delete_resource()->deallocate(p, bytes, alignment); }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
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
    "replacement reconnects both sides and preserves fan-out after old block destruction"_test = [] {
        qa_edit::registerTestBlocks();
        gr::Graph flow;
        auto& source = flow.emplaceBlock<qa_edit::Source>();
        auto& middle = flow.emplaceBlock<qa_edit::Tunable>();
        auto& sinkA = flow.emplaceBlock<qa_edit::Sink>();
        auto& sinkB = flow.emplaceBlock<qa_edit::Sink>();
        auto& bypass = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, middle).has_value());
        expect(flow.connect<"out", "in">(middle, sinkA).has_value());
        expect(flow.connect(middle, gr::PortDefinition("out"), sinkB, gr::PortDefinition("in")).has_value());
        expect(flow.connect<"out", "in">(source, bypass).has_value());
        expect(fatal(flow.connectPendingEdges()));
        flow.edges()[1]._domainStr = "gpu:qa";
        flow.edges()[1]._domain = gr::ComputeDomain::parse(flow.edges()[1]._domainStr);
        auto* bypassSource = flow.edges()[3]._sourcePort;
        auto* bypassDestination = flow.edges()[3]._destinationPort;
        std::weak_ptr<gr::BlockModel> lifetime = flow.blocks()[1];
        auto [oldBlock, replacement] = flow.replaceBlock(middle.unique_name, gr::meta::type_name<qa_edit::Tunable>(), {{"gain", 2.0f}});
        auto& next = *static_cast<qa_edit::Tunable*>(replacement->raw());
        expect(!middle.in.isConnected()) << "the retired reader must not throttle the source";
        expect(!middle.out.isConnected()) << "consumers must leave the retired writer";
        replacement->initDynamicPorts();
        oldBlock.reset();
        expect(lifetime.expired()) << "no scheduler or external shared_ptr may hide destruction";
        expect(eq(flow.blocks().size(), 5UZ));
        expect(eq(flow.edges().size(), 4UZ));
        expect(flow.edges()[1]._domain.backend.data() == flow.edges()[1]._domainStr.data() + 4);
        expect(eq(flow.edges()[1]._domain.backend, std::string_view("qa")));
        bool valid = true;
        for (const auto& edge : flow.edges()) {
            auto src = edge.sourceBlock()->dynamicOutputPort(edge.sourcePortDefinition());
            auto dst = edge.destinationBlock()->dynamicInputPort(edge.destinationPortDefinition());
            const bool matches = src && dst && edge._sourcePort == *src && edge._destinationPort == *dst;
            expect(matches) << "edge caches must name live replacement ports";
            valid = valid && matches;
        }
        // Safe on the broken baseline too: report stale identities before dereferencing caches.
        if (valid) {
            expect(eq(flow.edges()[1].nReaders(), 2UZ));
            expect(eq(flow.edges()[0].nWriters(), 1UZ));
        }
        expect(flow.edges()[3]._sourcePort == bypassSource);
        expect(flow.edges()[3]._destinationPort == bypassDestination);
        expect(eq(source.out.nReaders(), 2UZ));
        expect(eq(next.in.nWriters(), 1UZ));
        expect(eq(next.out.nReaders(), 2UZ));
        expect(eq(source.out.tagWriter().nReaders(), 2UZ));
        expect(eq(next.out.tagWriter().nReaders(), 2UZ));
        expect(eq(next.in.tagReader().nWriters(), 1UZ));
        {
            auto data = source.out.streamWriter().reserve(1UZ);
            data[0] = 7.0f;
            data.publish(1UZ);
        }
        expect(eq(next.in.streamReader().available(), 1UZ));
        expect(eq(bypass.in.streamReader().available(), 1UZ));
        {
            auto data = next.out.streamWriter().reserve(1UZ);
            data[0] = 14.0f;
            data.publish(1UZ);
        }
        for (auto* sink : {&sinkA, &sinkB}) {
            expect(eq(sink->in.streamReader().available(), 1UZ));
            if (sink->in.streamReader().available() == 1UZ) {
                auto data = sink->in.streamReader().get();
                expect(eq(data[0], 14.0f));
            }
        }
    };

    "replacement resolves named and indexed collection ports and refuses incompatible ports"_test = [] {
        using Ports = qa_edit::ReplacementPorts<float>;
        gr::BlockRegistry registry;
        gr::SchedulerRegistry schedulers;
        std::ignore = registry.insert<Ports>();
        std::ignore = registry.insert<qa_edit::ReplacementPorts<float, 1UZ>>();
        std::ignore = registry.insert<qa_edit::ReplacementPorts<double>>();
        std::ignore = registry.insert<qa_edit::Source>();
        gr::PluginLoader loader(registry, schedulers, {});
        for (const bool named : {false, true}) {
            gr::Graph flow(loader);
            auto& source = flow.emplaceBlock<qa_edit::Source>();
            auto& middle = flow.emplaceBlock<Ports>();
            auto& sink = flow.emplaceBlock<qa_edit::Sink>();
            const gr::PortDefinition input = named ? gr::PortDefinition("in#1") : gr::PortDefinition(0UZ, 1UZ);
            const gr::PortDefinition output = named ? gr::PortDefinition("out#1") : gr::PortDefinition(0UZ, 1UZ);
            expect(flow.connect(source, gr::PortDefinition("out"), middle, input).has_value());
            expect(flow.connect(middle, output, sink, gr::PortDefinition("in")).has_value());
            expect(fatal(flow.connectPendingEdges()));
            auto original = flow.blocks()[1];
            const std::vector<gr::Edge> edges(flow.edges().begin(), flow.edges().end());
            for (auto type : {gr::meta::type_name<qa_edit::ReplacementPorts<float, 1UZ>>(), gr::meta::type_name<qa_edit::ReplacementPorts<double>>(), gr::meta::type_name<qa_edit::Source>()}) {
                expect(throws<gr::exception>([&] { std::ignore = flow.replaceBlock(original->uniqueName(), type, {}); })) << type;
                expect(eq(flow.blocks().size(), 3UZ));
                expect(std::ranges::find(flow.blocks(), original) != flow.blocks().end());
                expect(std::ranges::equal(flow.edges(), edges));
                expect(eq(middle.in[1].nWriters(), 1UZ));
                expect(eq(middle.out[1].nReaders(), 1UZ));
            }
            // On baseline the first invalid replacement already removed the original.
            if (std::ranges::find(flow.blocks(), original) == flow.blocks().end()) { continue; }
            auto [oldBlock, replacement] = flow.replaceBlock(original->uniqueName(), gr::meta::type_name<Ports>(), {});
            auto& next = *static_cast<Ports*>(replacement->raw());
            expect(eq(next.in[1].nWriters(), 1UZ));
            expect(eq(next.out[1].nReaders(), 1UZ));
            expect(!next.in[0].isConnected());
            expect(!next.out[0].isConnected());
        }
    };

    "replacement permits edge reader inspection after destruction"_test = [] {
        qa_edit::registerTestBlocks();
        gr::Graph flow;
        auto& source = flow.emplaceBlock<qa_edit::Source>();
        auto& middle = flow.emplaceBlock<qa_edit::Tunable>();
        auto& sink = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, middle).has_value());
        expect(flow.connect<"out", "in">(middle, sink).has_value());
        expect(fatal(flow.connectPendingEdges()));
        std::weak_ptr<gr::BlockModel> lifetime = flow.blocks()[1];
        auto [oldBlock, replacement] = flow.replaceBlock(middle.unique_name, gr::meta::type_name<qa_edit::Tunable>(), {});
        replacement->initDynamicPorts();
        oldBlock.reset();
        expect(fatal(lifetime.expired()));
        // Deliberately exercise introspection after destruction; ASan diagnoses the old cache.
        expect(eq(flow.edges()[1].nReaders(), 1UZ));
        expect(eq(flow.edges()[0].nWriters(), 1UZ));
    };

    "replacement rejects a live input stream span before exchanging handlers"_test = [] {
        qa_edit::registerTestBlocks();
        gr::Graph flow;
        auto& source = flow.emplaceBlock<qa_edit::Source>();
        auto& middle = flow.emplaceBlock<qa_edit::Tunable>();
        auto& sink = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, middle).has_value());
        expect(flow.connect<"out", "in">(middle, sink).has_value());
        expect(fatal(flow.connectPendingEdges()));
        {
            auto data = source.out.streamWriter().reserve(3UZ);
            data[0] = 1.0f;
            data[1] = 2.0f;
            data[2] = 3.0f;
            data.publish(3UZ);
        }
        const auto original = flow.blocks()[1];
        const std::vector<gr::Edge> edges(flow.edges().begin(), flow.edges().end());
        {
            auto held = middle.in.streamReader().get();
            expect(eq(held.size(), 3UZ));
            expect(throws<gr::exception>([&] { std::ignore = flow.replaceBlock(original->uniqueName(), gr::meta::type_name<qa_edit::Tunable>(), {}); }));
            expect(flow.blocks()[1] == original);
            expect(std::ranges::equal(flow.edges(), edges));
            expect(middle.in.isConnected());
            expect(eq(middle.in.streamReader().available(), 3UZ));
            expect(held.consume(2UZ)) << "the span remains owned by A after rejection";
        }
        expect(eq(middle.in.streamReader().position(), 2UZ));
        expect(eq(middle.in.streamReader().available(), 1UZ));
        auto [retired, replacement] = flow.replaceBlock(original->uniqueName(), gr::meta::type_name<qa_edit::Tunable>(), {});
        auto& next = *static_cast<qa_edit::Tunable*>(replacement->raw());
        expect(eq(next.in.streamReader().position(), 2UZ));
        expect(eq(next.in.streamReader().available(), 1UZ));
    };

    "replacement rejects a live tag span before exchanging handlers"_test = [] {
        qa_edit::registerTestBlocks();
        gr::Graph flow;
        auto& source = flow.emplaceBlock<qa_edit::Source>();
        auto& middle = flow.emplaceBlock<qa_edit::Tunable>();
        auto& sink = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, middle).has_value());
        expect(flow.connect<"out", "in">(middle, sink).has_value());
        expect(fatal(flow.connectPendingEdges()));
        source.out.publishTag(gr::property_map{{"held", true}}, 0UZ);
        const auto original = flow.blocks()[1];
        const std::vector<gr::Edge> edges(flow.edges().begin(), flow.edges().end());
        {
            auto held = middle.in.tagReader().get();
            expect(eq(held.size(), 1UZ));
            expect(held[0].map.at("held") == true);
            expect(throws<gr::exception>([&] { std::ignore = flow.replaceBlock(original->uniqueName(), gr::meta::type_name<qa_edit::Tunable>(), {}); }));
            expect(flow.blocks()[1] == original);
            expect(std::ranges::equal(flow.edges(), edges));
            expect(eq(middle.in.tagReader().available(), 1UZ));
            expect(held.consume(1UZ)) << "the tag span remains owned by A after rejection";
        }
        expect(eq(middle.in.tagReader().available(), 0UZ));
        expect(nothrow([&] { std::ignore = flow.replaceBlock(original->uniqueName(), gr::meta::type_name<qa_edit::Tunable>(), {}); }));
    };

    "replacement rejects a live output span before exchanging handlers"_test = [] {
        qa_edit::registerTestBlocks();
        gr::Graph flow;
        auto& source = flow.emplaceBlock<qa_edit::Source>();
        auto& middle = flow.emplaceBlock<qa_edit::Tunable>();
        auto& sink = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, middle).has_value());
        expect(flow.connect<"out", "in">(middle, sink).has_value());
        expect(fatal(flow.connectPendingEdges()));
        const auto original = flow.blocks()[1];
        const std::vector<gr::Edge> edges(flow.edges().begin(), flow.edges().end());
        {
            auto held = middle.out.template reserve<gr::SpanReleasePolicy::ProcessNone>(1UZ);
            held[0] = 17.0f;
            expect(throws<gr::exception>([&] { std::ignore = flow.replaceBlock(original->uniqueName(), gr::meta::type_name<qa_edit::Tunable>(), {}); }));
            expect(flow.blocks()[1] == original);
            expect(std::ranges::equal(flow.edges(), edges));
            expect(middle.out.isConnected());
            held.publish(1UZ);
        }
        expect(eq(sink.in.streamReader().available(), 1UZ));
        auto [retired, replacement] = flow.replaceBlock(original->uniqueName(), gr::meta::type_name<qa_edit::Tunable>(), {});
        auto data = sink.in.streamReader().get();
        expect(eq(data[0], 17.0f));
    };

    "replacement preflights every affected endpoint for live spans"_test = [] {
        using Ports = qa_edit::ReplacementPorts<float>;
        gr::BlockRegistry registry;
        gr::SchedulerRegistry schedulers;
        std::ignore = registry.insert<Ports>();
        std::ignore = registry.insert<qa_edit::Source>();
        gr::PluginLoader loader(registry, schedulers, {});
        gr::Graph flow(loader);
        auto& sourceA = flow.emplaceBlock<qa_edit::Source>();
        auto& sourceB = flow.emplaceBlock<qa_edit::Source>();
        auto& middle = flow.emplaceBlock<Ports>();
        expect(flow.connect(sourceA, gr::PortDefinition("out"), middle, gr::PortDefinition("in#0")).has_value());
        expect(flow.connect(sourceB, gr::PortDefinition("out"), middle, gr::PortDefinition("in#1")).has_value());
        expect(fatal(flow.connectPendingEdges()));
        const auto original = flow.blocks()[2];
        const std::vector<gr::Edge> edges(flow.edges().begin(), flow.edges().end());
        {
            auto held = middle.in[1].streamReader().get();
            expect(throws<gr::exception>([&] { std::ignore = flow.replaceBlock(original->uniqueName(), gr::meta::type_name<Ports>(), {}); }));
            expect(flow.blocks()[2] == original);
            expect(std::ranges::equal(flow.edges(), edges));
            expect(eq(sourceA.out.nReaders(), 1UZ));
            expect(eq(sourceB.out.nReaders(), 1UZ));
            expect(middle.in[0].isConnected());
            expect(middle.in[1].isConnected());
        }
        expect(nothrow([&] { std::ignore = flow.replaceBlock(original->uniqueName(), gr::meta::type_name<Ports>(), {}); }));
    };

    "replacement rolls back a failed reconnection without removing the original"_test = [] {
        using Ports = qa_edit::ReplacementPorts<float>;
        gr::BlockRegistry registry;
        gr::SchedulerRegistry schedulers;
        std::ignore = registry.insert<Ports>();
        std::ignore = registry.insert<qa_edit::FailingReplacement>();
        gr::PluginLoader loader(registry, schedulers, {});
        qa_edit::FailingResource resource;
        gr::Graph flow(loader);
        auto& source = flow.emplaceBlock<qa_edit::Source>();
        auto& middle = flow.emplaceBlock<Ports>();
        auto& sinkA = flow.emplaceBlock<qa_edit::Sink>();
        auto& sinkB = flow.emplaceBlock<qa_edit::Sink>();
        auto& sinkC = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect(source, gr::PortDefinition("out"), middle, gr::PortDefinition("in#1")).has_value());
        expect(flow.connect(middle, gr::PortDefinition("out#1"), sinkA, gr::PortDefinition("in")).has_value());
        expect(flow.connect(middle, gr::PortDefinition("out#0"), sinkB, gr::PortDefinition("in"), {.dataResource = &resource}).has_value());
        expect(flow.connect(middle, gr::PortDefinition("out#1"), sinkC, gr::PortDefinition("in")).has_value());
        expect(fatal(flow.connectPendingEdges()));
        flow.edges()[1]._domainStr = "gpu:qa";
        flow.edges()[1]._domain = gr::ComputeDomain::parse(flow.edges()[1]._domainStr);
        const auto original = flow.blocks()[1];
        const std::vector<gr::Edge> edges(flow.edges().begin(), flow.edges().end());
        {
            auto data = source.out.streamWriter().reserve(3UZ);
            std::ranges::fill(data, 11.0f);
            data.publish(3UZ);
        }
        source.out.publishTag(gr::property_map{{"history", true}}, 1UZ);
        source.out.publishTag(gr::property_map{{static_cast<std::pmr::string>(gr::tag::END_OF_STREAM), true}}, 3UZ);
        {
            auto data = middle.in[1].streamReader().get();
            expect(data.consume(1UZ));
        }
        for (auto& output : middle.out) {
            auto data = output.streamWriter().reserve(3UZ);
            std::ranges::fill(data, 17.0f);
            data.publish(3UZ);
            output.publishTag(gr::property_map{{"history", true}}, 0UZ);
            output.publishTag(gr::property_map{{static_cast<std::pmr::string>(gr::tag::END_OF_STREAM), true}}, 3UZ);
        }
        resource.fail = true; // ordinary connected buffers must not be reallocated; optional sizing fails after transfer
        expect(throws<gr::exception>([&] { std::ignore = flow.replaceBlock(original->uniqueName(), gr::meta::type_name<qa_edit::FailingReplacement>(), {}); }));
        expect(flow.blocks()[1] == original);
        expect(std::ranges::equal(flow.edges(), edges));
        expect(flow.edges()[1]._domain.backend.data() == flow.edges()[1]._domainStr.data() + 4);
        expect(eq(source.out.nReaders(), 1UZ));
        expect(eq(middle.in[1].streamReader().position(), 1UZ));
        expect(eq(middle.in[1].streamReader().available(), 2UZ));
        expect(eq(middle.in[1].tagReader().available(), 2UZ));
        for (auto* sink : {&sinkA, &sinkB, &sinkC}) {
            expect(eq(sink->in.streamReader().available(), 3UZ)) << "failed replacement must preserve queued samples";
            expect(eq(sink->in.tagReader().available(), 2UZ)) << "failed replacement must preserve tags and EOS";
            auto data = sink->in.streamReader().get();
            expect(std::ranges::all_of(data, [](float value) { return value == 17.0f; }));
        }
        // Independent fan-out cursors survive the subsequent successful transfer too.
        {
            auto data = sinkA.in.streamReader().get();
            expect(data.consume(1UZ));
        }
        auto [retired, replacement] = flow.replaceBlock(original->uniqueName(), gr::meta::type_name<Ports>(), {});
        expect(flow.blocks()[1] == replacement);
        auto& next = *static_cast<Ports*>(replacement->raw());
        expect(eq(next.in[1].streamReader().position(), 1UZ));
        expect(eq(next.in[1].streamReader().available(), 2UZ));
        expect(eq(next.in[1].tagReader().available(), 2UZ));
        expect(!middle.in[1].isConnected());
        expect(!middle.out[0].isConnected());
        expect(!middle.out[1].isConnected());
        for (auto* sink : {&sinkA, &sinkB, &sinkC}) {
            expect(eq(sink->in.streamReader().available(), sink == &sinkA ? 2UZ : 3UZ));
            auto data = sink->in.streamReader().get();
            expect(std::ranges::all_of(data, [](float value) { return value == 17.0f; }));
            expect(data.consume(data.size()));
            auto tags = sink->in.tagReader().get();
            expect(fatal(eq(tags.size(), 2UZ)));
            expect(eq(tags[0].index, 0UZ));
            expect(tags[0].map.at("history") == true);
            expect(eq(tags[1].index, 3UZ));
            expect(tags[1].map.at(static_cast<std::pmr::string>(gr::tag::END_OF_STREAM)) == true);
            expect(tags.consume(tags.size()));
        }
        for (auto* sink : {&sinkA, &sinkB, &sinkC}) {
            expect(eq(sink->in.streamReader().available(), 0UZ));
            expect(eq(sink->in.tagReader().available(), 0UZ));
        }
    };

    "replacement refuses mixed definitions that collapse distinct inputs"_test = [] {
        gr::BlockRegistry registry;
        gr::SchedulerRegistry schedulers;
        std::ignore = registry.insert<qa_edit::ReorderedInputs>();
        gr::PluginLoader loader(registry, schedulers, {});
        gr::Graph flow(loader);
        auto& sourceA = flow.emplaceBlock<qa_edit::Source>();
        auto& sourceB = flow.emplaceBlock<qa_edit::Source>();
        auto& middle = flow.emplaceBlock<qa_edit::PairedInputs>();
        expect(flow.connect(sourceA, gr::PortDefinition("out"), middle, gr::PortDefinition(0UZ)).has_value());
        expect(flow.connect(sourceB, gr::PortDefinition("out"), middle, gr::PortDefinition("second")).has_value());
        expect(fatal(flow.connectPendingEdges()));
        auto original = flow.blocks()[2];
        expect(throws<gr::exception>([&] { std::ignore = flow.replaceBlock(original->uniqueName(), gr::meta::type_name<qa_edit::ReorderedInputs>(), {}); }));
        expect(flow.blocks()[2] == original);
        expect(eq(middle.first.nWriters(), 1UZ));
        expect(eq(middle.second.nWriters(), 1UZ));
        expect(eq(sourceA.out.nReaders(), 1UZ));
        expect(eq(sourceB.out.nReaders(), 1UZ));
    };

    "replacement sizes an optional output with its connected sibling"_test = [] {
        gr::BlockRegistry registry;
        gr::SchedulerRegistry schedulers;
        std::ignore = registry.insert<qa_edit::ReplacementMonitor>();
        gr::PluginLoader loader(registry, schedulers, {});
        gr::Graph flow(loader);
        auto& source = flow.emplaceBlock<qa_edit::Source>();
        auto& middle = flow.emplaceBlock<qa_edit::Tunable>();
        auto& sink = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, middle).has_value());
        expect(flow.connect<"out", "in">(middle, sink).has_value());
        expect(fatal(flow.connectPendingEdges()));
        auto [oldBlock, replacement] = flow.replaceBlock(middle.unique_name, gr::meta::type_name<qa_edit::ReplacementMonitor>(), {});
        auto& next = *static_cast<qa_edit::ReplacementMonitor*>(replacement->raw());
        expect(eq(next.monitor.bufferSize(), next.out.bufferSize()));
    };

    "replacement preserves pending edges"_test = [] {
        qa_edit::registerTestBlocks();
        gr::Graph flow;
        auto& source = flow.emplaceBlock<qa_edit::Source>();
        auto& middle = flow.emplaceBlock<qa_edit::Tunable>();
        auto& sink = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(source, middle).has_value());
        expect(flow.connect<"out", "in">(middle, sink).has_value());
        auto [oldBlock, replacement] = flow.replaceBlock(middle.unique_name, gr::meta::type_name<qa_edit::Tunable>(), {});
        oldBlock.reset();
        for (const auto& edge : flow.edges()) { expect(edge.state() == gr::Edge::EdgeState::WaitingToBeConnected); }
        expect(flow.connectPendingEdges());
        for (const auto& edge : flow.edges()) { expect(eq(edge.nReaders(), 1UZ)); expect(eq(edge.nWriters(), 1UZ)); }
    };

    "replacement keeps a staged edge pending when its ports have unrelated connections"_test = [] {
        qa_edit::registerTestBlocks();
        gr::Graph flow;
        auto&     sourceA = flow.emplaceBlock<qa_edit::Source>();
        auto&     sourceC = flow.emplaceBlock<qa_edit::Source>();
        auto&     sinkX   = flow.emplaceBlock<qa_edit::Sink>();
        auto&     sinkY   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.connect<"out", "in">(sourceA, sinkX).has_value());
        expect(flow.connect<"out", "in">(sourceC, sinkY).has_value());
        expect(fatal(flow.connectPendingEdges()));

        const gr::Edge removedEdge = flow.edges()[1];
        expect(flow.removeEdge(removedEdge));
        expect(eq(flow.edges().size(), 1UZ));
        expect(flow.connect<"out", "in">(sourceA, sinkY).has_value());
        expect(flow.edges().back().state() == gr::Edge::EdgeState::WaitingToBeConnected);

        const std::string sourceName(sourceA.unique_name);
        auto [retired, replacement] = flow.replaceBlock(sourceName, gr::meta::type_name<qa_edit::Source>(), {});
        retired.reset();
        auto& nextSource = *static_cast<qa_edit::Source*>(replacement->raw());
        auto  outputPort = replacement->dynamicOutputPort(gr::PortDefinition("out"));

        expect(flow.blocks()[0] == replacement) << "B must occupy A's graph slot";
        expect(fatal(eq(flow.edges().size(), 2UZ)));
        expect(flow.edges()[0].state() == gr::Edge::EdgeState::Connected) << "the established B -> X edge must stay connected";
        expect(flow.edges()[1].state() == gr::Edge::EdgeState::WaitingToBeConnected) << "the staged B -> Y edge must remain pending";
        expect(fatal(outputPort.has_value()));
        expect(flow.edges()[0]._sourcePort == *outputPort) << "the connected edge retained a stale pointer into A";
        expect(flow.edges()[1]._sourcePort == *outputPort) << "the pending edge retained a stale pointer into A";
        expect(eq(nextSource.out.nReaders(), 1UZ)) << "only X is attached before pending-edge processing";

        expect(flow.connectPendingEdges());
        expect(flow.edges()[1].state() == gr::Edge::EdgeState::Connected);
        expect(eq(nextSource.out.nReaders(), 2UZ)) << "B must have the X and Y readers exactly once";
        expect(eq(sinkX.in.nWriters(), 1UZ));
        expect(eq(sinkY.in.nWriters(), 1UZ));

        {
            auto samples = nextSource.out.streamWriter().reserve(1UZ);
            samples[0]   = 42.0f;
            samples.publish(1UZ);
        }
        expect(eq(sinkX.in.streamReader().available(), 1UZ));
        expect(eq(sinkY.in.streamReader().available(), 1UZ)) << "Y remained attached to C instead of B";
        {
            auto samples = sinkX.in.streamReader().get();
            expect(fatal(eq(samples.size(), 1UZ)));
            expect(eq(samples[0], 42.0f));
            expect(samples.consume(1UZ));
        }
        if (sinkY.in.streamReader().available() == 1UZ) {
            auto samples = sinkY.in.streamReader().get();
            expect(fatal(eq(samples.size(), 1UZ)));
            expect(eq(samples[0], 42.0f));
            expect(samples.consume(1UZ));
        }
        expect(eq(sinkX.in.streamReader().available(), 0UZ));
        expect(eq(sinkY.in.streamReader().available(), 0UZ));
    };

    "replacement recognizes the exact connection established by emplaceEdge"_test = [] {
        qa_edit::registerTestBlocks();
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_edit::Source>();
        auto&     sink   = flow.emplaceBlock<qa_edit::Sink>();
        expect(flow.emplaceEdge(source.unique_name, "out", sink.unique_name, "in", gr::undefined_size, 0, "directly wired").has_value());
        expect(fatal(eq(flow.edges().size(), 1UZ)));
        expect(flow.edges()[0].state() == gr::Edge::EdgeState::Connected);
        expect(flow.edges()[0]._sourcePort == *flow.blocks()[0]->dynamicOutputPort("out"));
        expect(flow.edges()[0]._destinationPort == *flow.blocks()[1]->dynamicInputPort("in"));

        {
            auto samples = source.out.streamWriter().reserve(2UZ);
            samples[0]   = 31.0f;
            samples[1]   = 32.0f;
            samples.publish(2UZ);
        }
        const std::size_t queuedTagIndex = source.out.streamWriter().position() + 1UZ;
        const std::size_t eosTagIndex    = source.out.streamWriter().position() + 2UZ;
        source.out.publishTag(gr::property_map{{"queued", true}}, 1UZ);
        source.out.publishTag(gr::property_map{{static_cast<std::pmr::string>(gr::tag::END_OF_STREAM), true}}, 2UZ);

        const std::string sourceName(source.unique_name);
        const std::string sinkName(sink.unique_name);
        auto [oldSource, replacementSource] = flow.replaceBlock(sourceName, gr::meta::type_name<qa_edit::Source>(), {});
        oldSource.reset();
        auto [oldSink, replacementSink] = flow.replaceBlock(sinkName, gr::meta::type_name<qa_edit::Sink>(), {});
        oldSink.reset();

        auto& nextSource = *static_cast<qa_edit::Source*>(replacementSource->raw());
        auto& nextSink   = *static_cast<qa_edit::Sink*>(replacementSink->raw());
        expect(flow.edges()[0].state() == gr::Edge::EdgeState::Connected);
        expect(flow.edges()[0]._sourcePort == *replacementSource->dynamicOutputPort("out"));
        expect(flow.edges()[0]._destinationPort == *replacementSink->dynamicInputPort("in"));
        expect(eq(nextSource.out.nReaders(), 1UZ));
        expect(eq(nextSink.in.nWriters(), 1UZ));
        expect(flow.connectPendingEdges()) << "a connected emplaceEdge must not create a duplicate reader";
        expect(eq(nextSource.out.nReaders(), 1UZ));

        auto samples = nextSink.in.streamReader().get();
        expect(fatal(eq(samples.size(), 2UZ)));
        expect(eq(samples[0], 31.0f));
        expect(eq(samples[1], 32.0f));
        auto tags = nextSink.in.tagReader().get();
        expect(fatal(eq(tags.size(), 2UZ)));
        expect(tags[0].map.at("queued") == true);
        expect(eq(tags[0].index, queuedTagIndex));
        expect(tags[1].map.at(static_cast<std::pmr::string>(gr::tag::END_OF_STREAM)) == true);
        expect(eq(tags[1].index, eosTagIndex));
    };

    "replacement refuses an owner of exported port aliases"_test = [] {
        qa_edit::registerTestBlocks();
        gr::GraphWrapper<gr::Graph> wrapper;
        auto& flow = *wrapper.graph();
        auto& middle = flow.emplaceBlock<qa_edit::Tunable>();
        const auto original = flow.blocks()[0];
        expect(wrapper.exportPort(true, middle.unique_name, gr::PortDirection::OUTPUT, "out", "out").has_value());
        expect(throws<gr::exception>([&] { std::ignore = flow.replaceBlock(middle.unique_name, gr::meta::type_name<qa_edit::Tunable>(), {}); }));
        expect(flow.blocks()[0] == original);
        expect(*wrapper.dynamicOutputPort("out").value() == *original->dynamicOutputPort("out").value());
    };

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

    "connected replacement transfers handlers across a loaded plugin boundary"_test = [] {
        gr::Graph flow;
        const auto& source = flow.emplaceBlock("good::fixed_source<float32>", {});
        const auto& sink   = flow.emplaceBlock("good::cout_sink<float32>", {});
        expect(flow.connect(source, gr::PortDefinition("out"), sink, gr::PortDefinition("in")).has_value());
        expect(fatal(flow.connectPendingEdges()));

        const std::string sourceName(source->uniqueName());
        const std::string sinkName(sink->uniqueName());
        expect(nothrow([&] { std::ignore = flow.replaceBlock(sourceName, "good::fixed_source<float32>", {}); }));
        expect(nothrow([&] { std::ignore = flow.replaceBlock(sinkName, "good::cout_sink<float32>", {}); }));
        expect(eq(flow.edges().front().nReaders(), 1UZ));
        expect(eq(flow.edges().front().nWriters(), 1UZ));
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
};

int main() { /* tests are statically registered */ }
