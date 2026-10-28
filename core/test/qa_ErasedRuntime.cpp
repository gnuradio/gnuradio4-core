#include <boost/ut.hpp>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockModel.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Runtime.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/YamlPmt.hpp>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstdlib>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "build_configure.hpp"

/**
 * A graph built and run through gr::Runtime by name, against the same graph built with the typed API. Every writable
 * member is settable through the erased path, and the two arms deliver identical samples and tags.
 *
 * gnuradio4-core carries no standard block library. The blocks are defined and registered here.
 */

namespace qa_runtime {

using namespace gr;
using boost::ut::expect;

struct TagRecord {
    std::size_t  index{};
    property_map map{};
};

struct Collected {
    std::vector<float>     samples;
    std::vector<TagRecord> tags;
};

inline std::mutex& collectorMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::map<std::string, Collected>& collector() {
    static std::map<std::string, Collected> collected;
    return collected;
}

inline std::map<std::string, std::size_t>& counted() {
    static std::map<std::string, std::size_t> nCounted;
    return nCounted;
}

inline Collected takeCollected(const std::string& sinkName) {
    std::lock_guard guard(collectorMutex());
    auto            entry = collector().find(sinkName);
    if (entry == collector().end()) {
        return {};
    }
    Collected result = std::move(entry->second);
    collector().erase(entry);
    return result;
}

inline std::string messageOf(const auto& expected) { return expected.has_value() ? std::string{} : expected.error().message; }

/// counts up from zero and tags every `tag_every` samples. Both arms see one deterministic stream.
struct RampSource : Block<RampSource> {
    PortOut<float> out;

    Annotated<gr::Size_t, "n_samples", Doc<"samples to emit before finishing">>            n_samples = 4096U;
    Annotated<gr::Size_t, "tag_every", Doc<"tag period in samples, 0 to disable tagging">> tag_every = 512U;

    GR_MAKE_REFLECTABLE(RampSource, out, n_samples, tag_every);

    gr::Size_t _emitted = 0U;

    explicit RampSource(property_map init = {}) : Block<RampSource>(std::move(init)) {}

    work::Status processBulk(OutputSpanLike auto& outSpan) {
        if (_emitted >= n_samples) {
            outSpan.publish(0UZ);
            return work::Status::DONE;
        }
        const std::size_t n = std::min(outSpan.size(), static_cast<std::size_t>(n_samples - _emitted));
        if (n == 0UZ) {
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        for (std::size_t i = 0UZ; i < n; ++i) {
            const gr::Size_t index = _emitted + static_cast<gr::Size_t>(i);
            outSpan[i]             = static_cast<float>(index);
            if (tag_every > 0U && index % tag_every == 0U) {
                outSpan.publishTag(property_map{{"trigger_name", std::format("t{}", index)}}, i);
            }
        }
        _emitted += static_cast<gr::Size_t>(n);
        outSpan.publish(n);
        return work::Status::OK;
    }
};

struct Scale : Block<Scale> {
    PortIn<float>  in;
    PortOut<float> out;

    Annotated<float, "gain">              gain    = 1.0f;
    Annotated<std::string, "label">       label   = "scale";
    Annotated<bool, "enabled">            enabled = true;
    Annotated<std::vector<float>, "taps"> taps    = std::vector<float>{1.0f};

    GR_MAKE_REFLECTABLE(Scale, in, out, gain, label, enabled, taps);

    explicit Scale(property_map init = {}) : Block<Scale>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return enabled ? value * gain : value; }
};

/// the shape of a multi-input arithmetic block, whose inputs are one collection port addressed as "in#0"
struct SumInputs : Block<SumInputs> {
    std::vector<PortIn<float>> in;
    PortOut<float>             out;

    Annotated<gr::Size_t, "n_inputs", Limits<1U, 8U>> n_inputs = 2U;

    GR_MAKE_REFLECTABLE(SumInputs, in, out, n_inputs);

    explicit SumInputs(property_map init = {}) : Block<SumInputs>(std::move(init)) { in.resize(n_inputs); }

    void settingsChanged(const property_map& oldSettings, const property_map& newSettings) {
        if (newSettings.contains("n_inputs") && oldSettings.at("n_inputs") != newSettings.at("n_inputs")) {
            in.resize(n_inputs);
        }
    }

    template<InputSpanLike TInSpan>
    work::Status processBulk(const std::span<TInSpan>& inSpans, OutputSpanLike auto& outSpan) const {
        std::copy(inSpans[0].begin(), inSpans[0].end(), outSpan.begin());
        for (std::size_t port = 1UZ; port < inSpans.size(); ++port) {
            std::transform(outSpan.begin(), outSpan.end(), inSpans[port].begin(), outSpan.begin(), std::plus<float>{});
        }
        return work::Status::OK;
    }
};

struct RecordingSink : Block<RecordingSink> {
    PortIn<float> in;

    GR_MAKE_REFLECTABLE(RecordingSink, in);

    explicit RecordingSink(property_map init = {}) : Block<RecordingSink>(std::move(init)) {}

    work::Status processBulk(InputSpanLike auto& inSpan) {
        const std::size_t n = inSpan.size();
        {
            std::lock_guard guard(collectorMutex());
            Collected&      collected = collector()[std::string(this->name)];
            collected.samples.insert(collected.samples.end(), inSpan.begin(), inSpan.begin() + static_cast<std::ptrdiff_t>(n));
            for (const Tag& tag : inSpan.rawTags) {
                collected.tags.push_back(TagRecord{tag.index, tag.map});
            }
        }
        inSpan.consumeTags(n);
        std::ignore = inSpan.consume(n);
        return work::Status::OK;
    }
};

/// publishes zeros until the run is stopped
struct EndlessSource : Block<EndlessSource> {
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(EndlessSource, out);

    explicit EndlessSource(property_map init = {}) : Block<EndlessSource>(std::move(init)) {}

    work::Status processBulk(OutputSpanLike auto& outSpan) {
        std::fill(outSpan.begin(), outSpan.end(), 0.0f);
        outSpan.publish(outSpan.size());
        return work::Status::OK;
    }
};

/// counts the samples it consumes and keeps none. An endless source can feed it for a whole case.
struct CountingSink : Block<CountingSink> {
    PortIn<float> in;

    GR_MAKE_REFLECTABLE(CountingSink, in);

    explicit CountingSink(property_map init = {}) : Block<CountingSink>(std::move(init)) {}

    work::Status processBulk(InputSpanLike auto& inSpan) {
        const std::size_t n = inSpan.size();
        {
            std::lock_guard guard(collectorMutex());
            counted()[std::string(this->name)] += n;
        }
        std::ignore = inSpan.consume(n);
        return work::Status::OK;
    }
};

/// counts the samples it consumes in a readable member, which the block writes while it runs
struct TallySink : Block<TallySink> {
    PortIn<float> in;

    Annotated<gr::Size_t, "n_consumed", Doc<"samples consumed">> n_consumed = 0U;

    GR_MAKE_REFLECTABLE(TallySink, in, n_consumed);

    explicit TallySink(property_map init = {}) : Block<TallySink>(std::move(init)) {}

    work::Status processBulk(InputSpanLike auto& inSpan) {
        const std::size_t n = inSpan.size();
        n_consumed          = n_consumed.value + static_cast<gr::Size_t>(n);
        std::ignore         = inSpan.consume(n);
        return work::Status::OK;
    }
};

/// a source whose start() throws. Every run of its graph fails.
struct FailingStartSource : Block<FailingStartSource> {
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(FailingStartSource, out);

    explicit FailingStartSource(property_map init = {}) : Block<FailingStartSource>(std::move(init)) {}

    void start() { throw gr::exception("the source refused to start"); }

    work::Status processBulk(OutputSpanLike auto& outSpan) {
        outSpan.publish(0UZ);
        return work::Status::DONE;
    }
};

/// holds each GatedStopSink's stop() until a case opens it, for at most ten seconds, and counts the stops it held
struct StopGate {
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    isOpen = false;
    std::size_t             nStops = 0UZ;
};

inline StopGate& stopGate() {
    static StopGate gate;
    return gate;
}

/// counts the samples it consumes as CountingSink does, and its stop() returns only once stopGate() opens
struct GatedStopSink : Block<GatedStopSink> {
    PortIn<float> in;

    GR_MAKE_REFLECTABLE(GatedStopSink, in);

    explicit GatedStopSink(property_map init = {}) : Block<GatedStopSink>(std::move(init)) {}

    void stop() {
        StopGate&        gate = stopGate();
        std::unique_lock lock(gate.mutex);
        ++gate.nStops;
        gate.changed.notify_all();
        std::ignore = gate.changed.wait_for(lock, std::chrono::seconds(10), [&gate] { return gate.isOpen; });
    }

    work::Status processBulk(InputSpanLike auto& inSpan) {
        const std::size_t n = inSpan.size();
        {
            std::lock_guard guard(collectorMutex());
            counted()[std::string(this->name)] += n;
        }
        std::ignore = inSpan.consume(n);
        return work::Status::OK;
    }
};

inline void registerTestBlocks() {
    static const bool registered = [] {
        BlockRegistry& registry = globalBlockRegistry();
        return registry.insert<RampSource>("=qa::RampSource") && registry.insert<Scale>("=qa::Scale")                        //
               && registry.insert<RecordingSink>("=qa::RecordingSink") && registry.insert<SumInputs>("=qa::SumInputs")       //
               && registry.insert<EndlessSource>("=qa::EndlessSource") && registry.insert<CountingSink>("=qa::CountingSink") //
               && registry.insert<FailingStartSource>("=qa::FailingStartSource") && registry.insert<TallySink>("=qa::TallySink");
    }();
    expect(registered) << "the test blocks must reach the global registry";
}

/// the same three-block chain, built with the typed API
inline gr::Graph typedChain(std::string_view sinkName, gr::Size_t nSamples, float gain) {
    gr::Graph flow;
    auto&     source = flow.emplaceBlock<RampSource>({{"name", std::string("source")}, {"n_samples", nSamples}});
    auto&     scale  = flow.emplaceBlock<Scale>({{"name", std::string("scale")}, {"gain", gain}});
    auto&     sink   = flow.emplaceBlock<RecordingSink>({{"name", std::string(sinkName)}});
    expect(flow.connect<"out", "in">(source, scale).has_value());
    expect(flow.connect<"out", "in">(scale, sink).has_value());
    return flow;
}

inline void runTyped(gr::Graph&& flow) {
    gr::scheduler::Simple<> scheduler;
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.runAndWait().has_value());
}

/// the same three-block chain, built through the entry
inline std::expected<gr::Runtime, gr::RuntimeError> erasedChain(std::string_view sinkName, gr::Size_t nSamples, float gain) {
    RuntimeGraph graph;
    auto         source = graph.emplace("qa::RampSource", "source", {{"n_samples", nSamples}});
    auto         scale  = graph.emplace("qa::Scale", "scale", {{"gain", gain}});
    auto         sink   = graph.emplace("qa::RecordingSink", sinkName);
    expect(source.has_value() && scale.has_value() && sink.has_value());
    expect(graph.connect(*source, "out", *scale, "in").has_value());
    expect(graph.connect(*scale, "out", *sink, "in").has_value());
    return Runtime::create(std::move(graph));
}

/// an endless source feeding a sink that counts and keeps nothing
inline std::expected<gr::Runtime, gr::RuntimeError> endlessChain(std::string_view sinkName) {
    RuntimeGraph graph;
    auto         source = graph.emplace("qa::EndlessSource", "source");
    auto         sink   = graph.emplace("qa::CountingSink", sinkName);
    expect(source.has_value() && sink.has_value());
    expect(graph.connect(*source, "out", *sink, "in").has_value());
    return Runtime::create(std::move(graph));
}

inline std::size_t countedSize(const std::string& sinkName) {
    std::lock_guard guard(collectorMutex());
    const auto      entry = counted().find(sinkName);
    return entry == counted().end() ? 0UZ : entry->second;
}

/// waits until the sink has counted more than `nBefore` samples, for at most three seconds, and returns its count
inline std::size_t awaitCountAbove(const std::string& sinkName, std::size_t nBefore) {
    const auto started = std::chrono::steady_clock::now();
    while (countedSize(sinkName) <= nBefore && std::chrono::steady_clock::now() - started < std::chrono::seconds(3)) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    return countedSize(sinkName);
}

/// closes the gate and clears its count
inline void closeStopGate() {
    StopGate&       gate = stopGate();
    std::lock_guard lock(gate.mutex);
    gate.isOpen = false;
    gate.nStops = 0UZ;
}

inline void openStopGate() {
    StopGate&       gate = stopGate();
    std::lock_guard lock(gate.mutex);
    gate.isOpen = true;
    gate.changed.notify_all();
}

/// waits until the gate has held `nStops` stops, for at most three seconds, and returns how many it held
inline std::size_t awaitHeldStops(std::size_t nStops) {
    StopGate&        gate = stopGate();
    std::unique_lock lock(gate.mutex);
    std::ignore = gate.changed.wait_for(lock, std::chrono::seconds(3), [&gate, nStops] { return gate.nStops >= nStops; });
    return gate.nStops;
}

inline bool sameTags(const std::vector<TagRecord>& lhs, const std::vector<TagRecord>& rhs) {
    return std::ranges::equal(lhs, rhs, [](const TagRecord& a, const TagRecord& b) { return a.index == b.index && a.map == b.map; });
}

} // namespace qa_runtime

#ifdef INTERNAL_ENABLE_BLOCK_PLUGINS
namespace qa_runtime {

inline bool setEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
    return ::_putenv_s(name, value.c_str()) == 0;
#else
    return ::setenv(name, value.c_str(), 1) == 0;
#endif
}

} // namespace qa_runtime

/**
 * A graph over a loader the program builds, which reads that loader and not the process's one.
 *
 * The suites of this file run in the order they appear, and this one runs first. The process's loader reads
 * GNURADIO4_PLUGIN_DIRECTORIES once, when a case first uses it. The case names a directory that does not exist before
 * that. The process's loader then holds no plugin.
 */
const boost::ut::suite<"a program's own loader"> programLoaderTests = [] {
    using namespace boost::ut;
    using namespace gr;
    using qa_runtime::messageOf;

    "a document naming a plugin block loads through the program's loader and not the process's, and the graph, its runtime and its handles keep that loader"_test = [] {
        constexpr std::string_view kSourceKey       = "good::fixed_source<float32>";
        constexpr std::string_view kDocument        = "blocks:\n  - id: good::fixed_source<float32>\n    parameters:\n      name: source\n";
        const std::string          missingDirectory = std::string(TESTS_BINARY_PATH) + "/no_such_plugin_directory";
        expect(fatal(qa_runtime::setEnvironment("GNURADIO4_PLUGIN_DIRECTORIES", missingDirectory)));
        expect(fatal(!std::ranges::contains(globalPluginLoader().availableBlocks(), kSourceKey))) << "the process's loader holds the plugin block already";

        auto loader = std::make_shared<PluginLoader>(globalBlockRegistry(), globalSchedulerRegistry(), std::vector<std::string>{std::string(TESTS_BINARY_PATH) + "/plugins"});
        expect(fatal(std::ranges::contains(loader->availableBlocks(), kSourceKey))) << "the program's loader holds no plugin block";

        const auto throughProcess = RuntimeGraph::fromYaml(nullptr, kDocument);
        expect(fatal(!throughProcess.has_value())) << "the process's loader resolved a block only the program's loader holds";
        expect(throughProcess.error().message.contains(kSourceKey)) << throughProcess.error().message;

        const std::vector<std::string> schedulers = loader->availableSchedulers();
        const auto                     scheduler  = std::ranges::find_if(schedulers, [](const std::string& name) { return name.contains("GoodMathScheduler"); });
        expect(fatal(scheduler != schedulers.end())) << "the program's loader holds no plugin scheduler";
        expect(!Runtime::create(RuntimeGraph{}, *scheduler).has_value()) << "the process's loader built the plugin scheduler";

        const std::weak_ptr<PluginLoader> watched = loader;
        BlockHandle                       pluginBlock;
        BlockHandle                       pluginScheduler;
        {
            auto graph = RuntimeGraph::fromYaml(std::move(loader), kDocument);
            expect(fatal(graph.has_value())) << messageOf(graph);
            const auto found = graph->find("source");
            expect(fatal(found.has_value())) << "the loaded block is not in the graph";
            pluginBlock = *found;
            expect(!watched.expired()) << "the graph does not keep its loader";

            auto runtime = Runtime::create(std::move(*graph), *scheduler);
            expect(fatal(runtime.has_value())) << messageOf(runtime);
            expect(!watched.expired()) << "the runtime does not keep its graph's loader";
            pluginScheduler = runtime->scheduler();
        }
        // the plugin's code stays mapped while a handle holds one of its objects
        expect(fatal(!watched.expired())) << "the handles do not keep the loader of their graph";
        expect(eq(pluginBlock.name(), std::string("source")));
        expect(pluginBlock.typeName().contains("fixed_source")) << pluginBlock.typeName();
        expect(!pluginBlock.get().empty()) << "the plugin block's handle reads no settings";
        expect(!pluginScheduler.writableMembers().empty()) << "the plugin scheduler's handle reads no settings";

        pluginBlock = BlockHandle{};
        expect(!watched.expired()) << "the scheduler's handle does not keep the loader";
        pluginScheduler = BlockHandle{};
        expect(watched.expired()) << "the loader outlives the graph, the runtime and the handles that held it";
    };
};
#endif

const boost::ut::suite<"erased runtime"> erasedRuntimeTests = [] {
    using namespace boost::ut;
    using namespace gr;
    using namespace qa_runtime;

    "a graph built by name runs and produces the expected stream"_test = [] {
        registerTestBlocks();
        auto runtime = erasedChain("basic", 1024U, 2.0f);
        expect(fatal(runtime.has_value())) << messageOf(runtime);
        expect(runtime->runAndWait().has_value());

        const Collected collected = takeCollected("basic");
        expect(fatal(eq(collected.samples.size(), 1024UZ)));
        expect(eq(collected.samples.front(), 0.0f));
        expect(eq(collected.samples.back(), 2.0f * 1023.0f));
    };

    "an unknown type and a mistyped port name are reported, not thrown"_test = [] {
        registerTestBlocks();
        RuntimeGraph graph;

        const auto missing = graph.emplace("qa::NoSuchBlock", "nope");
        expect(fatal(!missing.has_value()));
        expect(missing.error().message.contains("qa::NoSuchBlock")) << missing.error().message;

        const auto source = graph.emplace("qa::RampSource", "source");
        const auto sink   = graph.emplace("qa::RecordingSink", "sink");
        expect(fatal(source.has_value() && sink.has_value()));

        const auto typo = graph.connect(*source, "outt", *sink, "in");
        expect(fatal(!typo.has_value()));
        expect(typo.error().message.contains("[out]")) << "the refusal lists the ports the block has: " << typo.error().message;
    };

    "a setting the block does not declare is emplace()'s error, and nothing throws"_test = [] {
        registerTestBlocks();
        using Emplaced = std::expected<BlockHandle, RuntimeError>;
        RuntimeGraph graph;

        std::optional<Emplaced> misspelled;
        expect(nothrow([&] { misspelled.emplace(graph.emplace("qa::Scale", "scale", {{"gian", 2.0f}})); })) << "the refusal left emplace() as an exception";
        expect(fatal(misspelled.has_value() && !misspelled->has_value())) << "emplace() accepted a key the block does not declare";
        expect(misspelled->error().message.contains("gian")) << misspelled->error().message;
        expect(eq(misspelled->error().where, std::string("RuntimeGraph::emplace")));
        expect(graph.blocks().empty()) << "a refused block stayed in the graph";
    };

    "a block's port names are read from its handle, also after its graph moved into a runtime"_test = [] {
        registerTestBlocks();
        RuntimeGraph graph;
        const auto   source = graph.emplace("qa::RampSource", "source", {{"n_samples", gr::Size_t{16U}}});
        const auto   scale  = graph.emplace("qa::Scale", "scale");
        const auto   sink   = graph.emplace("qa::RecordingSink", "port-names");
        expect(fatal(source.has_value() && scale.has_value() && sink.has_value()));

        expect(scale->inputPortNames() == std::vector<std::string>{"in"});
        expect(scale->outputPortNames() == std::vector<std::string>{"out"});
        expect(source->inputPortNames().empty());
        expect(graph.connect(*source, source->outputPortNames().front(), *scale, scale->inputPortNames().front()).has_value());
        expect(graph.connect(*scale, "out", *sink, "in").has_value());

        auto runtime = Runtime::create(std::move(graph));
        expect(fatal(runtime.has_value())) << messageOf(runtime);
        expect(scale->inputPortNames() == std::vector<std::string>{"in"}) << "the handle lost its port names when the graph moved";
        expect(runtime->runAndWait().has_value());
        expect(eq(takeCollected("port-names").samples.size(), 16UZ));
    };

    // A block the caller built is indistinguishable from an emplaced one. A factory therefore keeps its own typed
    // pointer and passes the graph the erased one.
    "an added block behaves exactly as an emplaced one"_test = [] {
        registerTestBlocks();
        runTyped(typedChain("add-reference", 4096U, 3.0f));
        const Collected reference = takeCollected("add-reference");

        auto         scaleBlock = std::make_shared<BlockWrapper<Scale>>(property_map{{"gain", 3.0f}});
        Scale* const typedScale = static_cast<Scale*>(scaleBlock->raw());
        auto         sinkBlock  = std::make_shared<BlockWrapper<RecordingSink>>();

        RuntimeGraph graph;
        auto         source = graph.emplace("qa::RampSource", "source", {{"n_samples", gr::Size_t{4096U}}});
        auto         scale  = graph.add(std::shared_ptr<BlockModel>(scaleBlock), "scale");
        auto         sink   = graph.add(std::shared_ptr<BlockModel>(sinkBlock), "add-erased");
        expect(fatal(source.has_value() && scale.has_value() && sink.has_value())) << messageOf(scale);

        expect(eq(scale->name(), std::string("scale"))) << "add() must name the block before init() runs";
        expect(!scale->uniqueName().empty());
        expect(scale->typeName().contains("Scale")) << scale->typeName();

        // a staged value lands on the caller's own object when the run applies it
        expect(scale->get().contains(convert_string_domain(std::string("gain"))));
        expect(scale->setStaged(property_map{{"taps", std::vector<float>{2.0f}}}).has_value());
        expect(scale->setStaged(property_map{{"label", std::string("adopted")}}).has_value());

        expect(graph.connect(*source, "out", *scale, "in").has_value());
        expect(graph.connect(*scale, "out", *sink, "in").has_value());
        expect(graph.find(scale->uniqueName(), RuntimeGraph::Recursive::No).has_value());
        expect(eq(graph.blocks().size(), 3UZ));

        auto runtime = Runtime::create(std::move(graph));
        expect(fatal(runtime.has_value())) << messageOf(runtime);
        expect(runtime->runAndWait().has_value());
        expect(runtime->state() == Runtime::State::Stopped) << "the run must settle stopped";

        const Collected added = takeCollected("add-erased");
        expect(reference.samples == added.samples) << "an added block changed the stream";
        expect(sameTags(reference.tags, added.tags)) << "an added block changed the tag sequence";
        expect(eq(typedScale->label.value, std::string("adopted")));
        expect(eq(typedScale->gain.value, 3.0f));
    };

    "add refuses a null block"_test = [] {
        RuntimeGraph graph;
        const auto   added = graph.add(nullptr);
        expect(fatal(!added.has_value()));
        expect(added.error().message.contains("null")) << added.error().message;
    };

    "a collection input port is addressed element by element"_test = [] {
        registerTestBlocks();
        RuntimeGraph graph;
        auto         source = graph.emplace("qa::RampSource", "source", {{"n_samples", gr::Size_t{1024U}}});
        auto         sum    = graph.emplace("qa::SumInputs", "sum", {{"n_inputs", gr::Size_t{2U}}});
        auto         sink   = graph.emplace("qa::RecordingSink", "collection");
        expect(fatal(source.has_value() && sum.has_value() && sink.has_value()));

        expect(sum->inputPortNames() == std::vector<std::string>{"in#0", "in#1"}) << "the listed names must be the ones connect accepts";

        // the base name of a collection resolves to nothing, here and when the graph starts alike
        const auto bare = graph.connect(*source, "out", *sum, "in");
        expect(fatal(!bare.has_value())) << "the base name of a collection must be refused at connect(), not at start()";
        expect(bare.error().message.contains("in#0")) << bare.error().message;

        expect(graph.connect(*source, "out", *sum, "in#0").has_value());
        expect(graph.connect(*source, "out", *sum, "in#1").has_value());
        expect(graph.connect(*sum, "out", *sink, "in").has_value());

        auto runtime = Runtime::create(std::move(graph));
        expect(fatal(runtime.has_value())) << messageOf(runtime);
        expect(runtime->runAndWait().has_value());

        const Collected collected = takeCollected("collection");
        expect(fatal(eq(collected.samples.size(), 1024UZ)));
        expect(eq(collected.samples.back(), 2.0f * 1023.0f)) << "both elements of the collection must have carried the stream";
    };

    "every writable member of every registered block is read and staged through a handle"_test = [] {
        registerTestBlocks();

        std::vector<std::string> skipped;
        for (const std::string& key : globalBlockRegistry().keys()) {
            RuntimeGraph graph;
            auto         handle = graph.emplace(key, "probe");
            if (!handle.has_value()) {
                skipped.push_back(key);
                continue;
            }

            const std::set<std::string> writable = handle->writableMembers();
            const property_map          active   = handle->get();
            expect(!writable.empty()) << key << " reports no writable members";

            for (const std::string& member : writable) {
                const auto value = active.find(convert_string_domain(member));
                expect(value != active.end()) << std::format("{}: writable member '{}' is not in get()", key, member);
                if (value == active.end()) {
                    continue;
                }
                const auto staged = handle->setStaged(property_map{{convert_string_domain(member), value->second}});
                expect(staged.has_value()) << std::format("{}: writable member '{}' was refused: {}", key, member, messageOf(staged));
            }
        }
        expect(skipped.empty()) << std::format("registered but not constructible through the entry: {}", skipped);
    };

    "typed and erased graphs deliver identical samples and tags"_test = [] {
        registerTestBlocks();
        runTyped(typedChain("parity-typed", 4096U, 3.0f));
        const Collected fromTyped = takeCollected("parity-typed");

        auto runtime = erasedChain("parity-erased", 4096U, 3.0f);
        expect(fatal(runtime.has_value())) << messageOf(runtime);
        expect(runtime->runAndWait().has_value());
        const Collected fromErased = takeCollected("parity-erased");

        expect(eq(fromTyped.samples.size(), 4096UZ));
        expect(fromTyped.samples == fromErased.samples) << "sample streams differ";
        expect(!fromTyped.tags.empty()) << "the source must have tagged the stream";
        expect(sameTags(fromTyped.tags, fromErased.tags)) << "tag sequences differ";
    };

    "settings given to fromYaml replace the document's, and a name no block carries is an error"_test = [] {
        registerTestBlocks();
        constexpr std::string_view document = "blocks:\n  - id: qa::Scale\n    parameters:\n      name: scale\n      gain: 3.0\n      label: from-the-document\n";

        auto loaded = RuntimeGraph::fromYaml(nullptr, document, {{"scale", {{"gain", 5.0f}}}});
        expect(fatal(loaded.has_value())) << messageOf(loaded);
        const std::vector<BlockHandle> blocks = loaded->blocks();
        expect(fatal(eq(blocks.size(), 1UZ)));
        expect(blocks.front().get(std::string("gain")) == std::optional<pmt::Value>(5.0f));
        expect(blocks.front().get(std::string("label")) == std::optional<pmt::Value>(std::string("from-the-document")));

        std::optional<std::expected<RuntimeGraph, RuntimeError>> unknownName;
        expect(nothrow([&] { unknownName.emplace(RuntimeGraph::fromYaml(nullptr, document, {{"scal", {{"gain", 5.0f}}}})); }));
        expect(fatal(unknownName.has_value() && !unknownName->has_value()));
        expect(unknownName->error().message.contains("block 'scal'")) << unknownName->error().message;
        expect(eq(unknownName->error().where, std::string("RuntimeGraph::fromYaml")));
    };

    "a document the reader refuses is an error, never an exception"_test = [] {
        registerTestBlocks();
        using Loaded = std::expected<RuntimeGraph, RuntimeError>;

        // the quote on the fourth line never closes
        constexpr std::string_view malformed = "blocks:\n  - id: qa::Scale\n    parameters:\n      name: \"scale\n";
        std::optional<Loaded>      fromMalformed;
        expect(nothrow([&] { fromMalformed.emplace(RuntimeGraph::fromYaml(nullptr, malformed)); }));
        expect(fatal(fromMalformed.has_value() && !fromMalformed->has_value()));
        const auto parsed = pmt::yaml::deserialize(malformed);
        expect(fatal(!parsed.has_value()));
        const RuntimeError& parseError = fromMalformed->error();
        expect(parseError.message.starts_with(std::format("line {}, column {}: ", parsed.error().line, parsed.error().column))) << parseError.message;
        expect(parseError.message.contains(parsed.error().message)) << parseError.message;
        expect(eq(parseError.where, std::string("RuntimeGraph::fromYaml")));

        constexpr std::string_view unknownType = "blocks:\n  - id: qa::NoSuchBlock\n    parameters:\n      name: nope\n";
        std::optional<Loaded>      fromUnknownType;
        expect(nothrow([&] { fromUnknownType.emplace(RuntimeGraph::fromYaml(nullptr, unknownType)); }));
        expect(fatal(fromUnknownType.has_value() && !fromUnknownType->has_value()));
        expect(fromUnknownType->error().message.contains("qa::NoSuchBlock")) << fromUnknownType->error().message;
        expect(eq(fromUnknownType->error().where, std::string("RuntimeGraph::fromYaml")));
    };

    "the scheduler's own settings are read through its handle, which outlives the runtime"_test = [] {
        registerTestBlocks();
        const auto timeoutOf = [](const BlockHandle& scheduler) {
            const std::optional<pmt::Value> value = scheduler.get(std::string("timeout_ms"));
            return value.has_value() && value->get_if<gr::Size_t>() != nullptr ? std::optional<gr::Size_t>(*value->get_if<gr::Size_t>()) : std::nullopt;
        };

        auto withDefaults = Runtime::create(RuntimeGraph{});
        expect(fatal(withDefaults.has_value())) << messageOf(withDefaults);
        const std::optional<gr::Size_t> defaultTimeout = timeoutOf(withDefaults->scheduler());
        expect(fatal(defaultTimeout.has_value())) << "the scheduler handle reports no timeout_ms";

        const gr::Size_t chosenTimeout = *defaultTimeout + 23U;
        auto             runtime       = Runtime::create(RuntimeGraph{}, Runtime::kDefaultScheduler, {{"timeout_ms", chosenTimeout}});
        expect(fatal(runtime.has_value())) << messageOf(runtime);
        expect(timeoutOf(runtime->scheduler()) == chosenTimeout) << "the handle reports the timeout the scheduler was created with";

        BlockHandle outliving;
        {
            Runtime moved = std::move(*runtime);
            expect(!runtime->scheduler().valid()) << "a moved-from Runtime has a scheduler";
            outliving = moved.scheduler();
        }
        expect(fatal(outliving.valid())) << "a scheduler handle does not outlive its Runtime";
        expect(timeoutOf(outliving) == chosenTimeout) << "a scheduler handle reads other settings after its Runtime is destroyed";
    };

    "find() takes a unique name or a name, and refuses a name two blocks share"_test = [] {
        registerTestBlocks();
        RuntimeGraph graph;
        auto         scale  = graph.emplace("qa::Scale", "gain-stage");
        auto         first  = graph.emplace("qa::RecordingSink", "twin");
        auto         second = graph.emplace("qa::RecordingSink", "twin");
        expect(fatal(scale.has_value() && first.has_value() && second.has_value()));

        const auto byName = graph.find("gain-stage");
        expect(fatal(byName.has_value())) << "a name one block carries is not found";
        expect(eq(byName->uniqueName(), scale->uniqueName()));
        const auto byUniqueName = graph.find(second->uniqueName());
        expect(byUniqueName.has_value() && byUniqueName->uniqueName() == second->uniqueName()) << "a unique name does not find its block among blocks sharing a name";

        const auto shared = graph.find("twin");
        expect(fatal(!shared.has_value())) << "a name two blocks share found one of them";
        expect(shared.error().message.contains("'twin'")) << shared.error().message;
        const auto absent = graph.find("absent");
        expect(fatal(!absent.has_value())) << "a name no block carries found a block";
        expect(absent.error().message.contains("'absent'")) << absent.error().message;
    };

    "find() takes the unique_name a document gives a block"_test = [] {
        registerTestBlocks();
        constexpr std::string_view document = "blocks:\n"
                                              "  - id: qa::Scale\n    unique_name: left\n    parameters:\n      name: twin\n"
                                              "  - id: qa::Scale\n    unique_name: right\n    parameters:\n      name: twin\n";
        auto                       loaded   = RuntimeGraph::fromYaml(nullptr, document);
        expect(fatal(loaded.has_value())) << messageOf(loaded);

        const auto left  = loaded->find("left");
        const auto right = loaded->find("right");
        expect(fatal(left.has_value() && right.has_value())) << "a document's unique_name does not find its block";
        expect(left->uniqueName() != right->uniqueName()) << "two document names found one block";
        expect(eq(left->name(), std::string("twin")));
        expect(!loaded->find("twin").has_value()) << "a name two blocks share found one of them";
    };

    "blocks() without an argument lists the blocks of a nested graph, whose scheduler comes from a document"_test = [] {
        registerTestBlocks();
        const std::string document = std::format("blocks:\n  - id: SUBGRAPH\n    parameters:\n      name: group\n    scheduler:\n      id: {}\n"
                                                 "    graph:\n      blocks:\n        - id: qa::Scale\n          parameters:\n            name: inner\n",
            Runtime::kDefaultScheduler);
        auto              loaded   = RuntimeGraph::fromYaml(nullptr, document);
        expect(fatal(loaded.has_value())) << messageOf(loaded);

        const std::vector<BlockHandle> topLevel = loaded->blocks(RuntimeGraph::Recursive::No);
        expect(fatal(eq(topLevel.size(), 1UZ))) << "the document's top level holds one block";
        expect(eq(topLevel.front().name(), std::string("group")));

        const std::vector<BlockHandle> all = loaded->blocks();
        expect(eq(all.size(), topLevel.size() + 1UZ)) << "blocks() without an argument left out the nested block";
        expect(std::ranges::any_of(all, [](const BlockHandle& block) { return block.name() == "inner"; })) << "the nested block is not listed";
        expect(loaded->find("inner").has_value()) << "find() does not reach the nested block";
    };

    "name() returns a copy, and a rename the run applies is read after it"_test = [] {
        static_assert(std::same_as<decltype(std::declval<const BlockHandle&>().name()), std::string>);
        static_assert(std::same_as<decltype(std::declval<const BlockHandle&>().uniqueName()), std::string>);
        static_assert(std::same_as<decltype(std::declval<const BlockHandle&>().typeName()), std::string>);
        registerTestBlocks();
        RuntimeGraph graph;
        auto         source = graph.emplace("qa::RampSource", "source", {{"n_samples", gr::Size_t{16U}}});
        auto         scale  = graph.emplace("qa::Scale", "before");
        auto         sink   = graph.emplace("qa::RecordingSink", "renamed");
        expect(fatal(source.has_value() && scale.has_value() && sink.has_value()));
        expect(graph.connect(*source, "out", *scale, "in").has_value());
        expect(graph.connect(*scale, "out", *sink, "in").has_value());

        const auto nameBefore = scale->name();
        expect(fatal(scale->setStaged({{"name", std::string("after")}}).has_value()));
        auto runtime = Runtime::create(std::move(graph));
        expect(fatal(runtime.has_value())) << messageOf(runtime);
        expect(runtime->runAndWait().has_value());
        std::ignore = takeCollected("renamed");

        expect(eq(scale->name(), std::string("after"))) << "the run did not apply the staged name";
        expect(eq(nameBefore, std::string("before"))) << "a name read before the rename changed with the block";
    };

    "get() after a run reads the value a block wrote while it ran"_test = [] {
        registerTestBlocks();
        static constexpr gr::Size_t kSamples = 1000U;
        const auto                  tallyRun = [](std::string_view tallyName) {
            RuntimeGraph graph;
            auto         source = graph.emplace("qa::RampSource", "source", {{"n_samples", kSamples}});
            auto         tally  = graph.emplace("qa::TallySink", tallyName);
            expect(fatal(source.has_value() && tally.has_value()));
            expect(graph.connect(*source, "out", *tally, "in").has_value());
            expect(tally->get(std::string("n_consumed")) == std::optional<pmt::Value>(gr::Size_t{0U}));
            auto runtime = Runtime::create(std::move(graph));
            expect(fatal(runtime.has_value())) << messageOf(runtime);
            return std::pair{std::move(*runtime), *tally};
        };

        auto [foreground, foregroundTally]          = tallyRun("tally-foreground");
        const std::expected<void, RuntimeError> ran = foreground.runAndWait();
        expect(fatal(ran.has_value())) << messageOf(ran);
        expect(foregroundTally.get(std::string("n_consumed")) == std::optional<pmt::Value>(kSamples)) << "runAndWait() returned before the settings held the count the block wrote";

        auto [background, backgroundTally] = tallyRun("tally-background");
        expect(fatal(background.start().has_value()));
        expect(fatal(background.waitFor(std::chrono::seconds(10)))) << "the finite run did not end";
        expect(backgroundTally.get(std::string("n_consumed")) == std::optional<pmt::Value>(kSamples)) << "the run's end did not refresh the settings";
    };
};

const boost::ut::suite<"background run"> backgroundRunTests = [] {
    using namespace boost::ut;
    using namespace gr;
    using namespace qa_runtime;
    using namespace std::chrono_literals;

    "a finite graph started in the background ends by itself, and its result is success"_test = [] {
        registerTestBlocks();
        auto runtime = erasedChain("background-finite", 1024U, 2.0f);
        expect(fatal(runtime.has_value())) << messageOf(runtime);
        const std::expected<void, RuntimeError> started = runtime->start();
        expect(fatal(started.has_value())) << messageOf(started);
        expect(fatal(runtime->waitFor(10s))) << "the finite run did not end";
        const std::expected<void, RuntimeError> result = runtime->result();
        expect(result.has_value()) << messageOf(result);
        expect(runtime->state() == Runtime::State::Stopped) << "a run that ended by itself settles stopped";
        expect(eq(takeCollected("background-finite").samples.size(), 1024UZ));
    };

    "start() returns its refusal as an expected error"_test = [] {
        static_assert(std::same_as<decltype(std::declval<Runtime&>().start()), std::expected<void, RuntimeError>>);
        registerTestBlocks();
        auto runtime = endlessChain("start-refused");
        expect(fatal(runtime.has_value())) << messageOf(runtime);
        expect(fatal(runtime->start().has_value()));

        const std::expected<void, RuntimeError> second = runtime->start();
        expect(fatal(!second.has_value())) << "a second start() during the run was accepted";
        expect(second.error().message.contains("a run is in progress")) << second.error().message;
        expect(eq(second.error().where, std::string("Runtime::start")));
        runtime->stop();
    };

    "start() returns once the scheduler runs"_test = [] {
        registerTestBlocks();
        auto runtime = endlessChain("start-running");
        expect(fatal(runtime.has_value())) << messageOf(runtime);
        for (std::size_t run = 0UZ; run < 3UZ; ++run) {
            expect(fatal(runtime->start().has_value()));
            expect(runtime->state() == Runtime::State::Running) << std::format("run {}: start() returned before the scheduler was running", run);
            runtime->stop();
        }
    };

    "a failed run's result carries the block's error"_test = [] {
        registerTestBlocks();
        RuntimeGraph graph;
        auto         source = graph.emplace("qa::FailingStartSource", "failing");
        auto         sink   = graph.emplace("qa::RecordingSink", "background-failed");
        expect(fatal(source.has_value() && sink.has_value()));
        expect(graph.connect(*source, "out", *sink, "in").has_value());
        const std::string sourceName(source->uniqueName());
        auto              runtime = Runtime::create(std::move(graph));
        expect(fatal(runtime.has_value())) << messageOf(runtime);

        expect(fatal(runtime->start().has_value()));
        expect(fatal(runtime->waitFor(10s))) << "the failing run did not end";
        const std::expected<void, RuntimeError> result = runtime->result();
        expect(fatal(!result.has_value())) << "a run whose block could not start reported success";
        expect(result.error().message.contains("the source refused to start")) << result.error().message;
        expect(result.error().message.contains(sourceName)) << result.error().message;
        expect(runtime->state() == Runtime::State::Error);
        runtime->stop();
        expect(runtime->state() == Runtime::State::Error) << "a stop after the failed run changed the state";
        expect(eq(takeCollected("background-failed").samples.size(), 0UZ));
    };

    "a stop before any run leaves the next run whole"_test = [] {
        registerTestBlocks();
        auto runtime = erasedChain("stopped-first", 1024U, 1.0f);
        expect(fatal(runtime.has_value())) << messageOf(runtime);
        runtime->stop();
        expect(runtime->state() == Runtime::State::Idle) << "a stop without a run changed the state";
        expect(runtime->runAndWait().has_value());
        expect(eq(takeCollected("stopped-first").samples.size(), 1024UZ)) << "the run after a stop without a run delivered less than the stream";
    };

    "a second stop and a stop after the run ended leave the scheduler stopped"_test = [] {
        registerTestBlocks();
        auto endless = endlessChain("stopped-endless");
        expect(fatal(endless.has_value())) << messageOf(endless);
        expect(fatal(endless->start().has_value()));
        expect(gt(awaitCountAbove("stopped-endless", 0UZ), 0UZ)) << "the endless run delivered nothing";
        endless->stop();
        expect(endless->waitFor(0ns)) << "stop() returned before the run ended";
        expect(endless->state() == Runtime::State::Stopped);
        endless->stop();
        expect(endless->state() == Runtime::State::Stopped) << "a second stop changed the state";
        expect(endless->result().has_value()) << "a stopped run is a successful run";

        auto foreground = erasedChain("stopped-foreground", 256U, 1.0f);
        expect(fatal(foreground.has_value())) << messageOf(foreground);
        expect(foreground->runAndWait().has_value());
        foreground->stop();
        expect(foreground->state() == Runtime::State::Stopped) << "a stop after runAndWait() returned changed the state";
        std::ignore = takeCollected("stopped-foreground");
    };

    "a run after a stop runs the graph again"_test = [] {
        registerTestBlocks();
        auto runtime = endlessChain("run-again");
        expect(fatal(runtime.has_value())) << messageOf(runtime);
        expect(fatal(runtime->start().has_value()));
        expect(gt(awaitCountAbove("run-again", 0UZ), 0UZ)) << "the first run delivered nothing";
        runtime->stop();
        const std::size_t nFirst = countedSize("run-again");
        expect(fatal(runtime->start().has_value())) << "a start after a stop was refused";
        expect(gt(awaitCountAbove("run-again", nFirst), nFirst)) << "the second run delivered nothing";
        runtime->stop();
        expect(runtime->state() == Runtime::State::Stopped);
        expect(runtime->result().has_value());
    };

    "a start or a foreground run while a run is in progress is refused"_test = [] {
        registerTestBlocks();
        auto runtime = endlessChain("in-progress");
        expect(fatal(runtime.has_value())) << messageOf(runtime);
        expect(fatal(runtime->start().has_value()));
        expect(gt(awaitCountAbove("in-progress", 0UZ), 0UZ)) << "the run delivered nothing";

        const std::expected<void, RuntimeError> foreground = runtime->runAndWait();
        expect(fatal(!foreground.has_value())) << "runAndWait() during the run was accepted";
        expect(foreground.error().message.contains("a run is in progress")) << foreground.error().message;
        expect(!runtime->waitFor(0ns)) << "a refused run ended the run in progress";
        expect(!runtime->result().has_value()) << "result() reported a run in progress as ended";

        runtime->stop();
        expect(runtime->result().has_value()) << "the refusal changed the result of the run it did not start";
    };

    "a timed wait returns false while the run goes on and true once it is stopped"_test = [] {
        registerTestBlocks();
        auto runtime = endlessChain("timed-wait");
        expect(fatal(runtime.has_value())) << messageOf(runtime);
        expect(fatal(runtime->start().has_value()));

        constexpr std::chrono::milliseconds kShortWait{20};
        const auto                          beforeShortWait = std::chrono::steady_clock::now();
        expect(!runtime->waitFor(kShortWait)) << "an endless run read as ended";
        expect(std::chrono::steady_clock::now() - beforeShortWait >= kShortWait) << "the timed wait returned before its timeout";

        constexpr std::chrono::seconds kLongWait{10};
        std::thread                    stopper([&runtime] { runtime->stop(); });
        const auto                     beforeLongWait = std::chrono::steady_clock::now();
        const bool                     ended          = runtime->waitFor(kLongWait);
        const auto                     waited         = std::chrono::steady_clock::now() - beforeLongWait;
        stopper.join();
        expect(ended) << "the wait timed out although another thread stopped the run";
        expect(waited < kLongWait) << "the wait did not return when the run ended";
        expect(runtime->waitFor(0ms)) << "a wait after the run ended returned false";
    };

    "a wait and a stop at the largest timeout wait for the end of the run"_test = [] {
        registerTestBlocks();
        constexpr std::chrono::nanoseconds kForever = std::chrono::nanoseconds::max();
        auto                               runtime  = endlessChain("largest-timeout");
        expect(fatal(runtime.has_value())) << messageOf(runtime);
        expect(runtime->waitFor(kForever)) << "a wait without a run returned false";

        expect(fatal(runtime->start().has_value()));
        std::thread stopper([&runtime] {
            std::ignore = awaitCountAbove("largest-timeout", 0UZ);
            runtime->stop();
        });
        const bool  ended = runtime->waitFor(kForever);
        stopper.join();
        expect(ended) << "a wait at the largest timeout returned before the run ended";

        expect(fatal(runtime->start().has_value()));
        expect(gt(awaitCountAbove("largest-timeout", countedSize("largest-timeout")), 0UZ));
        expect(runtime->stopFor(kForever)) << "a stop at the largest timeout returned before the run ended";
        expect(runtime->waitFor(0ns));
    };

    "a runtime destroyed during a run stops the run and joins its thread"_test = [] {
        registerTestBlocks();
        std::optional<Runtime> runtime;
        {
            auto created = endlessChain("destroyed");
            expect(fatal(created.has_value())) << messageOf(created);
            runtime.emplace(std::move(*created));
        }
        expect(fatal(runtime->start().has_value()));
        expect(gt(awaitCountAbove("destroyed", 0UZ), 0UZ)) << "the run delivered nothing";
        runtime.reset(); // a joinable thread destroyed unjoined terminates the process, and an unstopped run never ends
    };

    "a timed stop of an endless graph ends it and returns true"_test = [] {
        registerTestBlocks();
        auto runtime = endlessChain("timed-stop");
        expect(fatal(runtime.has_value())) << messageOf(runtime);
        expect(fatal(runtime->start().has_value()));
        expect(gt(awaitCountAbove("timed-stop", 0UZ), 0UZ)) << "the endless run delivered nothing";
        expect(runtime->stopFor(10s)) << "a timed stop did not end an endless run";
        expect(runtime->waitFor(0ns)) << "stopFor() returned true before the run ended";
        expect(runtime->state() == Runtime::State::Stopped);
        expect(runtime->result().has_value()) << "a stopped run is a successful run";
    };

    "a timed stop without a run returns true and changes nothing"_test = [] {
        registerTestBlocks();
        auto runtime = erasedChain("no-run", 1024U, 1.0f);
        expect(fatal(runtime.has_value())) << messageOf(runtime);
        expect(runtime->stopFor(0ns)) << "a timed stop before any run returned false";
        expect(runtime->state() == Runtime::State::Idle) << "a timed stop without a run changed the state";
        expect(runtime->runAndWait().has_value());
        expect(eq(takeCollected("no-run").samples.size(), 1024UZ)) << "the run after a timed stop without a run delivered less than the stream";
        expect(runtime->stopFor(0ns)) << "a timed stop after the run ended returned false";
        expect(runtime->state() == Runtime::State::Stopped) << "a timed stop after the run ended changed the state";
    };

    "a stop held in a block's stop() past the timeout returns false, and a later call waits for the same stop"_test = [] {
        registerTestBlocks();
        closeStopGate();
        RuntimeGraph graph;
        auto         source = graph.emplace("qa::EndlessSource", "source");
        auto         sink   = graph.add(std::make_shared<BlockWrapper<GatedStopSink>>(), "gated");
        expect(fatal(source.has_value() && sink.has_value()));
        expect(graph.connect(*source, "out", *sink, "in").has_value());
        auto runtime = Runtime::create(std::move(graph));
        expect(fatal(runtime.has_value())) << messageOf(runtime);
        expect(fatal(runtime->start().has_value()));
        expect(fatal(gt(awaitCountAbove("gated", 0UZ), 0UZ))) << "the gated sink consumed nothing, and its stop() would not run";

        constexpr std::chrono::milliseconds kTimeout{20};
        expect(!runtime->stopFor(kTimeout)) << "a stop held in a block's stop() read as ended";
        expect(eq(awaitHeldStops(1UZ), 1UZ)) << "the stop did not reach the block";
        const auto beforeLaterCall = std::chrono::steady_clock::now();
        expect(!runtime->stopFor(kTimeout)) << "a later call read the held stop as ended";
        expect(std::chrono::steady_clock::now() - beforeLaterCall < 5s) << "a later call waited for the held stop() past its own timeout";
        expect(!runtime->waitFor(0ns)) << "the run ended while its block was inside stop()";

        openStopGate();
        expect(runtime->stopFor(10s)) << "the run did not end once the block's stop() returned";
        expect(eq(awaitHeldStops(1UZ), 1UZ)) << "the block's stop() ran more than once";
        expect(runtime->state() == Runtime::State::Stopped);
        expect(runtime->result().has_value()) << "a stopped run is a successful run";
    };
};

const boost::ut::suite<"refusals return"> refusalTests = [] {
    using namespace boost::ut;
    using namespace gr;
    using namespace qa_runtime;

    "a staged value the block cannot convert is an error, not an exception"_test = [] {
        registerTestBlocks();
        RuntimeGraph graph;
        auto         scale = graph.emplace("qa::Scale", "scale");
        expect(fatal(scale.has_value()));
        std::optional<std::expected<void, RuntimeError>> refused;
        expect(nothrow([&] { refused.emplace(scale->setStaged({{"gain", std::string("loud")}})); }));
        expect(fatal(refused.has_value() && !refused->has_value())) << "a value that does not convert was taken";
        expect(refused->error().message.contains("gain")) << refused->error().message;
        expect(eq(refused->error().where, std::string("BlockHandle::setStaged")));
    };

    "a staged key the block does not declare is an error naming the key, and nothing is staged"_test = [] {
        registerTestBlocks();
        auto         scaleBlock = std::make_shared<BlockWrapper<Scale>>();
        RuntimeGraph graph;
        auto         scale = graph.add(std::shared_ptr<BlockModel>(scaleBlock), "scale");
        expect(fatal(scale.has_value())) << messageOf(scale);

        const std::expected<void, RuntimeError> refused = scale->setStaged({{"gain", 5.0f}, {"qa_undeclared", 1.0f}});
        expect(fatal(!refused.has_value())) << "a key the block does not declare was taken";
        expect(refused.error().message.contains("qa_undeclared")) << refused.error().message;
        expect(scaleBlock->settings().stagedParameters().empty()) << "a refused call staged the declared key beside the undeclared one";

        expect(scale->setStaged({{"gain", 5.0f}}).has_value()) << "a declared key alone is refused";
        expect(!scaleBlock->settings().stagedParameters().empty()) << "a declared key was not staged";
    };

    "a block built with a value it refuses is refused by add()"_test = [] {
        RuntimeGraph                                            graph;
        std::optional<std::expected<BlockHandle, RuntimeError>> refused;
        expect(nothrow([&] { refused.emplace(graph.add(std::make_shared<BlockWrapper<Scale>>(property_map{{"gain", std::string("loud")}}), "refused")); }));
        expect(fatal(refused.has_value() && !refused->has_value())) << "a block that refused its parameters was added";
        expect(graph.blocks().empty()) << "a refused block stayed in the graph";
    };

    "a scheduler setting that is misspelled or refused is refused by create()"_test = [] {
        registerTestBlocks();
        const auto misspelled = Runtime::create(RuntimeGraph{}, Runtime::kDefaultScheduler, property_map{{"timeout_msx", gr::Size_t{10U}}});
        expect(fatal(!misspelled.has_value())) << "a key the scheduler does not declare was dropped";
        expect(misspelled.error().message.contains("timeout_msx")) << "the refusal names the key: " << misspelled.error().message;

        RuntimeGraph kept;
        expect(fatal(kept.emplace("qa::Scale", "scale").has_value()));
        expect(!Runtime::create(std::move(kept), Runtime::kDefaultScheduler, property_map{{"timeout_msx", gr::Size_t{10U}}}).has_value());
        expect(eq(kept.blocks().size(), 1UZ)) << "a refused create() took the caller's graph";
        expect(Runtime::create(std::move(kept)).has_value()) << "the graph a refused create() left is not taken by a good one";

        std::optional<std::expected<Runtime, RuntimeError>> refused;
        expect(nothrow([&] { refused.emplace(Runtime::create(RuntimeGraph{}, Runtime::kDefaultScheduler, property_map{{"timeout_ms", std::string("soon")}})); }));
        expect(fatal(refused.has_value() && !refused->has_value())) << "a value the scheduler refuses was taken";

        expect(!Runtime::create(RuntimeGraph{}, "gr::scheduler::NoSuchScheduler").has_value()) << "an unknown scheduler type was built";
        expect(Runtime::create(RuntimeGraph{}, Runtime::kDefaultScheduler, property_map{{"timeout_ms", gr::Size_t{10U}}}).has_value()) << "a declared key with a good value is refused";
    };

    "emplace() refuses a scheduler type and names fromYaml() as the place for a nested scheduler"_test = [] {
        registerTestBlocks();
        RuntimeGraph graph;
        const auto   nested = graph.emplace(Runtime::kDefaultScheduler, "nested");
        expect(fatal(!nested.has_value())) << "emplace() built a scheduler as a nested block";
        expect(nested.error().message.contains("fromYaml()")) << nested.error().message;
        expect(graph.blocks().empty()) << "a refused scheduler stayed in the graph";
    };

    "the scheduler's handle refuses a staged setting and names create() as the place for it"_test = [] {
        registerTestBlocks();
        auto runtime = Runtime::create(RuntimeGraph{});
        expect(fatal(runtime.has_value())) << messageOf(runtime);
        BlockHandle                     scheduler = runtime->scheduler();
        const std::optional<pmt::Value> before    = scheduler.get(std::string("timeout_ms"));
        expect(fatal(before.has_value()));
        expect(scheduler.writableMembers().contains("timeout_ms")) << "the scheduler does not list its own setting";

        const std::expected<void, RuntimeError> refused = scheduler.setStaged({{"timeout_ms", gr::Size_t{77U}}});
        expect(fatal(!refused.has_value())) << "the scheduler's handle staged a setting the scheduler never applies";
        expect(refused.error().message.contains("Runtime::create()")) << refused.error().message;
        expect(scheduler.get(std::string("timeout_ms")) == before) << "the refused call changed the scheduler's setting";
    };
};

int main() { /* tests are statically registered */ }
