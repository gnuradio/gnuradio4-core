#include <boost/ut.hpp>

#include <gnuradio-4.0/BlockModel.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

/**
 * The GRC round trip: a graph saved by gr::saveGrc and read back by gr::loadGrc is the same graph.
 *
 * Blocks, settings, meta_information, collection ports, transparent subgraphs and exported ports all
 * travel through the YAML, and a graph that came back through it runs and terminates like the one it
 * was written from.
 *
 * gnuradio4-core carries no standard block library, so the blocks are defined and registered here.
 */

namespace qa_grc {

using namespace gr;
using boost::ut::eq;
using boost::ut::expect;

inline std::mutex& collectorMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::map<std::string, std::vector<float>>& collector() {
    static std::map<std::string, std::vector<float>> collected;
    return collected;
}

inline std::vector<float> takeCollected(const std::string& sinkName) {
    std::lock_guard guard(collectorMutex());
    auto            entry = collector().find(sinkName);
    if (entry == collector().end()) {
        return {};
    }
    std::vector<float> result = std::move(entry->second);
    collector().erase(entry);
    return result;
}

struct RampSource : Block<RampSource> {
    PortOut<float> out;

    Annotated<gr::Size_t, "n_samples", Doc<"samples to emit before finishing, 0 = never finish">> n_samples = 4096U;

    GR_MAKE_REFLECTABLE(RampSource, out, n_samples);

    gr::Size_t _emitted = 0U;

    explicit RampSource(property_map init = {}) : Block<RampSource>(std::move(init)) {}

    work::Status processBulk(OutputSpanLike auto& outSpan) {
        if (n_samples > 0U && _emitted >= n_samples) {
            outSpan.publish(0UZ);
            return work::Status::DONE;
        }
        const std::size_t room = n_samples > 0U ? std::min(outSpan.size(), static_cast<std::size_t>(n_samples - _emitted)) : outSpan.size();
        if (room == 0UZ) {
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        for (std::size_t i = 0UZ; i < room; ++i) {
            outSpan[i] = static_cast<float>(_emitted + static_cast<gr::Size_t>(i));
        }
        _emitted += static_cast<gr::Size_t>(room);
        outSpan.publish(room);
        return work::Status::OK;
    }
};

struct Scale : Block<Scale> {
    PortIn<float>  in;
    PortOut<float> out;

    Annotated<float, "gain", Doc<"output = gain * input">>        gain    = 1.0f;
    Annotated<std::string, "label">                               label   = "scale";
    Annotated<bool, "enabled">                                    enabled = true;
    Annotated<std::vector<float>, "taps", Doc<"carried, unused">> taps    = std::vector<float>{1.0f};

    GR_MAKE_REFLECTABLE(Scale, in, out, gain, label, enabled, taps);

    explicit Scale(property_map init = {}) : Block<Scale>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return enabled ? value * gain : value; }
};

/// a multi-input block: its inputs are one collection port, addressed "in#0" and "in#1"
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
        std::ranges::copy(inSpans[0], outSpan.begin());
        for (std::size_t port = 1UZ; port < inSpans.size(); ++port) {
            std::ranges::transform(outSpan, inSpans[port], outSpan.begin(), std::plus<float>{});
        }
        return work::Status::OK;
    }
};

struct RecordingSink : Block<RecordingSink> {
    PortIn<float> in;

    GR_MAKE_REFLECTABLE(RecordingSink, in);

    explicit RecordingSink(property_map init = {}) : Block<RecordingSink>(std::move(init)) {}

    work::Status processBulk(InputSpanLike auto& inSpan) {
        {
            std::lock_guard     guard(collectorMutex());
            std::vector<float>& samples = collector()[std::string(this->name)];
            samples.insert(samples.end(), inSpan.begin(), inSpan.end());
        }
        inSpan.consumeTags(inSpan.size());
        std::ignore = inSpan.consume(inSpan.size());
        return work::Status::OK;
    }
};

inline void registerTestBlocks() {
    static const bool registered = [] {
        BlockRegistry& registry = globalBlockRegistry();
        return registry.insert<RampSource>("=qa::RampSource") && registry.insert<Scale>("=qa::Scale") //
               && registry.insert<SumInputs>("=qa::SumInputs") && registry.insert<RecordingSink>("=qa::RecordingSink");
    }();
    expect(registered) << "the test blocks must reach the global registry";
}

inline void collectUniqueNames(const gr::Graph& graph, std::vector<std::string>& names) {
    for (const auto& block : graph.blocks()) {
        names.emplace_back(block->uniqueName());
        if (const gr::Graph* interior = block->graph(); interior != nullptr) {
            collectUniqueNames(*interior, names);
        }
    }
}

/**
 * The dump with every unique name replaced by the position of its block in the graph.
 *
 * Unique names are minted per process, so the same structure saved twice never produces the same text.
 * The result is the parsed document rather than its text: a property_map does not serialize its keys in
 * a stable order, so two dumps of the same graph differ as text and not as structure. Comparing the
 * parsed documents is the statement worth making about the blocks, their settings and their connections.
 */
inline gr::property_map canonicalGrc(PluginLoader& loader, const gr::Graph& graph) {
    std::string text = gr::saveGrc(loader, graph);

    std::vector<std::string> names;
    collectUniqueNames(graph, names);

    std::vector<std::pair<std::string, std::string>> substitutions;
    substitutions.reserve(names.size());
    for (std::size_t index = 0UZ; index < names.size(); ++index) {
        substitutions.emplace_back(names[index], std::format("<block{}>", index));
    }
    // longest first, so one unique name that is a prefix of another cannot claim its text
    std::ranges::stable_sort(substitutions, [](const auto& lhs, const auto& rhs) { return lhs.first.size() > rhs.first.size(); });

    for (const auto& [from, to] : substitutions) {
        for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
            text.replace(at, from.size(), to);
        }
    }

    auto parsed = pmt::yaml::deserialize(text);
    boost::ut::expect(parsed.has_value()) << "a dump this writer produced must parse";
    return parsed.value_or(gr::property_map{});
}

/// the graph, saved, loaded and saved again -- so the second document is what the reader made of the first
inline std::pair<gr::property_map, gr::property_map> roundTrip(PluginLoader& loader, const gr::Graph& graph) {
    gr::property_map before = canonicalGrc(loader, graph);
    auto             loaded = gr::loadGrc(loader, gr::saveGrc(loader, graph));
    return {std::move(before), canonicalGrc(loader, *loaded)};
}

/**
 * The first field the two documents disagree on, or an empty string when they agree.
 *
 * This is the comparison, not just a message for one: maps are compared key by key and sequences element
 * by element, so a container that carries the same values under different internal extents counts as
 * equal. pmt::Value::operator== does not, and a YAML round trip does not preserve that metadata.
 */
inline std::string describeDifference(const gr::property_map& lhs, const gr::property_map& rhs, const std::string& path = {});

inline std::string describeValueDifference(const pmt::Value& lhs, const pmt::Value& rhs, const std::string& path) {
    if (const auto lhsMap = lhs.get_if<gr::property_map>(), rhsMap = rhs.get_if<gr::property_map>(); lhsMap != nullptr && rhsMap != nullptr) {
        return describeDifference(*lhsMap, *rhsMap, path);
    }
    if (const auto lhsSeq = lhs.get_if<Tensor<pmt::Value>>(), rhsSeq = rhs.get_if<Tensor<pmt::Value>>(); lhsSeq != nullptr && rhsSeq != nullptr) {
        if (lhsSeq->size() != rhsSeq->size()) {
            return std::format("{}: {} entries became {}", path, lhsSeq->size(), rhsSeq->size());
        }
        for (std::size_t index = 0UZ; index < lhsSeq->size(); ++index) {
            if (std::string difference = describeValueDifference((*lhsSeq)[index], (*rhsSeq)[index], std::format("{}[{}]", path, index)); !difference.empty()) {
                return difference;
            }
        }
        return {};
    }
    if (lhs == rhs || std::format("{}", lhs) == std::format("{}", rhs)) {
        return {};
    }
    return std::format("{}: {} became {}", path, lhs, rhs);
}

inline std::string describeDifference(const gr::property_map& lhs, const gr::property_map& rhs, const std::string& path) {
    for (const auto& [key, value] : lhs) {
        const auto found = rhs.find(key);
        if (found == rhs.cend()) {
            return std::format("{}/{} was dropped", path, std::string(key));
        }
        if (std::string difference = describeValueDifference(value, found->second, std::format("{}/{}", path, std::string(key))); !difference.empty()) {
            return difference;
        }
    }
    for (const auto& [key, value] : rhs) {
        if (!lhs.contains(key)) {
            return std::format("{}/{} appeared", path, std::string(key));
        }
    }
    return {};
}

inline std::size_t countCategory(const gr::Graph& graph, block::Category category) {
    return static_cast<std::size_t>(std::ranges::count_if(graph.blocks(), [category](const auto& block) { return block->blockCategory() == category; }));
}

} // namespace qa_grc

const boost::ut::suite<"GRC round trip"> grcTests = [] {
    using namespace boost::ut;
    using namespace gr;
    using namespace qa_grc;

    "a flat graph is the same graph after a round trip"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<RampSource>({{"name", std::string("source")}, {"n_samples", 1024U}});
        auto&     scale  = flow.emplaceBlock<Scale>({{"name", std::string("scale")}, {"gain", 3.0f}, {"label", std::string("front")}, {"enabled", true}});
        auto&     sink   = flow.emplaceBlock<RecordingSink>({{"name", std::string("flat-sink")}});
        expect(flow.connect<"out", "in">(source, scale).has_value());
        expect(flow.connect<"out", "in">(scale, sink).has_value());

        const auto [before, after]   = roundTrip(loader, flow);
        const std::string difference = describeDifference(before, after);
        expect(difference.empty()) << difference;
    };

    "settings survive the round trip"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        gr::Graph flow;
        auto&     scale = flow.emplaceBlock<Scale>({{"name", std::string("scale")}, {"gain", 2.5f}, {"label", std::string("carried")}, {"enabled", false}});
        std::ignore     = scale;

        auto loaded = gr::loadGrc(loader, gr::saveGrc(loader, flow));
        expect(eq(loaded->blocks().size(), 1UZ));

        const property_map parameters = loaded->blocks().front()->settings().get();
        expect(parameters.at("gain") == pmt::Value(2.5f)) << "a float setting did not survive";
        expect(parameters.at("label") == pmt::Value(std::string("carried"))) << "a string setting did not survive";
        expect(parameters.at("enabled") == pmt::Value(false)) << "a bool setting did not survive";
    };

    // the writer keyed edges by name; two blocks may share one, and the second then claimed both ends
    "two blocks sharing a name keep their own edges"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<RampSource>({{"name", std::string("source")}, {"n_samples", 64U}});
        auto&     first  = flow.emplaceBlock<Scale>({{"name", std::string("stage")}, {"gain", 2.0f}});
        auto&     second = flow.emplaceBlock<Scale>({{"name", std::string("stage")}, {"gain", 5.0f}});
        auto&     sink   = flow.emplaceBlock<RecordingSink>({{"name", std::string("shared-name-sink")}});
        expect(flow.connect<"out", "in">(source, first).has_value());
        expect(flow.connect<"out", "in">(first, second).has_value());
        expect(flow.connect<"out", "in">(second, sink).has_value());

        auto loaded = gr::loadGrc(loader, gr::saveGrc(loader, flow));
        expect(eq(loaded->blocks().size(), 4UZ));
        expect(eq(loaded->edges().size(), 3UZ)) << "an edge was lost to the shared name";
        for (const Edge& edge : loaded->edges()) {
            expect(edge.sourceBlock() != edge.destinationBlock()) << "a shared name folded an edge onto one block";
        }

        gr::scheduler::Simple<> scheduler;
        expect(scheduler.exchange(std::move(loaded)).has_value());
        expect(scheduler.runAndWait().has_value());

        const std::vector<float> samples = takeCollected("shared-name-sink");
        expect(eq(samples.size(), 64UZ));
        expect(eq(samples.back(), 63.0f * 2.0f * 5.0f)) << "the two stages did not both run";
    };

    // the writer emitted a subgraph's name at the top level and no parameters key, and the reader
    // demanded parameters/name before it looked at the id
    "a subgraph written by saveGrc loads again"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<RampSource>({{"name", std::string("source")}, {"n_samples", 256U}});
        auto&     sink   = flow.emplaceBlock<RecordingSink>({{"name", std::string("nested-sink")}});

        auto  wrapper  = std::make_shared<GraphWrapper<gr::Graph>>();
        auto& interior = *wrapper->graph();
        auto& scale    = interior.emplaceBlock<Scale>({{"name", std::string("inner-scale")}, {"gain", 4.0f}});

        const std::shared_ptr<BlockModel>& subgraph = flow.addBlock(wrapper);
        subgraph->setName("inner");
        subgraph->metaInformation()["application:role"] = std::string("demodulator");
        scale.meta_information["application:disabled"]  = false;
        expect(wrapper->exportPort(true, std::string(scale.unique_name), PortDirection::INPUT, "in", "in").has_value());
        expect(wrapper->exportPort(true, std::string(scale.unique_name), PortDirection::OUTPUT, "out", "out").has_value());

        expect(flow.connect(flow.blocks()[0], PortDefinition{"out"}, subgraph, PortDefinition{"in"}).has_value());
        expect(flow.connect(subgraph, PortDefinition{"out"}, flow.blocks()[1], PortDefinition{"in"}).has_value());
        std::ignore = source;
        std::ignore = sink;

        const auto [before, after]   = roundTrip(loader, flow);
        const std::string difference = describeDifference(before, after);
        expect(difference.empty()) << difference;

        // the document is skimmable and deterministic: a block's id is the first thing under it
        const std::string dump = gr::saveGrc(loader, flow);
        expect(dump.find("id:") != std::string::npos && dump.find("id:") < dump.find("parameters:")) << "a block must lead with its id";
        expect(eq(dump, gr::saveGrc(loader, flow))) << "two saves of one graph must be byte-identical";

        auto loaded = gr::loadGrc(loader, gr::saveGrc(loader, flow));
        expect(eq(countCategory(*loaded, block::Category::TransparentBlockGroup), 1UZ)) << "the subgraph did not come back";

        const auto group = std::ranges::find_if(loaded->blocks(), [](const auto& block) { return block->blockCategory() == block::Category::TransparentBlockGroup; });
        expect(group != loaded->blocks().end());
        expect(eq((*group)->graph()->blocks().size(), 1UZ)) << "the subgraph's child did not come back";
        expect(eq((*group)->dynamicInputPorts().size(), 1UZ)) << "the exported input port did not come back";
        expect(eq((*group)->dynamicOutputPorts().size(), 1UZ)) << "the exported output port did not come back";
        expect(eq(loaded->edges().size(), 2UZ)) << "the edges onto the subgraph did not come back";

        const auto& groupMeta = (*group)->metaInformation();
        const auto  role      = groupMeta.find("application:role");
        expect(role != groupMeta.cend() && role->second == pmt::Value(std::string("demodulator"))) << "the subgraph's meta_information did not come back";
        const auto& innerMeta = (*group)->graph()->blocks().front()->metaInformation();
        const auto  disabled  = innerMeta.find("application:disabled");
        expect(disabled != innerMeta.cend() && disabled->second == pmt::Value(false)) << "an interior block's meta_information did not come back";
    };

    "a collection port survives the round trip"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        gr::Graph flow;
        auto&     left  = flow.emplaceBlock<RampSource>({{"name", std::string("left")}, {"n_samples", 128U}});
        auto&     right = flow.emplaceBlock<RampSource>({{"name", std::string("right")}, {"n_samples", 128U}});
        auto&     sum   = flow.emplaceBlock<SumInputs>({{"name", std::string("sum")}, {"n_inputs", 2U}});
        auto&     sink  = flow.emplaceBlock<RecordingSink>({{"name", std::string("collection-sink")}});
        expect(flow.connect(flow.blocks()[0], PortDefinition{"out"}, flow.blocks()[2], PortDefinition{"in#0"}).has_value());
        expect(flow.connect(flow.blocks()[1], PortDefinition{"out"}, flow.blocks()[2], PortDefinition{"in#1"}).has_value());
        expect(flow.connect<"out", "in">(sum, sink).has_value());
        std::ignore = left;
        std::ignore = right;

        const auto [before, after]   = roundTrip(loader, flow);
        const std::string difference = describeDifference(before, after);
        expect(difference.empty()) << difference;

        auto                    loaded = gr::loadGrc(loader, gr::saveGrc(loader, flow));
        gr::scheduler::Simple<> scheduler;
        expect(scheduler.exchange(std::move(loaded)).has_value());
        expect(scheduler.runAndWait().has_value());

        const std::vector<float> samples = takeCollected("collection-sink");
        expect(eq(samples.size(), 128UZ));
        expect(eq(samples.back(), 2.0f * 127.0f)) << "both collection elements must reach the sum";
    };

    // serialization reads the graph; committing what the caller staged is a side effect on a live receiver
    "saving a graph does not commit its staged parameters"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        gr::Graph flow;
        auto&     scale = flow.emplaceBlock<Scale>({{"name", std::string("scale")}, {"gain", 3.0f}});
        expect(scale.settings().setStaged({{"gain", 9.0f}}).empty()) << "the staged value must be accepted";
        expect(!scale.settings().stagedParameters().empty()) << "the value must be staged, not applied";

        const std::string dump = gr::saveGrc(loader, flow);

        expect(!scale.settings().stagedParameters().empty()) << "saveGrc committed the staged parameters";
        expect(eq(scale.gain.value, 3.0f)) << "saveGrc changed the graph it saved";

        auto loaded = gr::loadGrc(loader, dump);
        expect(eq(loaded->blocks().size(), 1UZ));
        expect(loaded->blocks().front()->settings().get().at("gain") == pmt::Value(9.0f)) << "the dump must carry what the block would use";
    };

    // the end-to-end statement: the graph that came back through the YAML runs, and delivers what the
    // flat chain it was written from delivers
    "a round-tripped subgraph delivers the flat chain's stream"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        {
            gr::Graph flat;
            auto&     source = flat.emplaceBlock<RampSource>({{"name", std::string("source")}, {"n_samples", 512U}});
            auto&     scale  = flat.emplaceBlock<Scale>({{"name", std::string("scale")}, {"gain", 2.0f}});
            auto&     sink   = flat.emplaceBlock<RecordingSink>({{"name", std::string("rt-flat")}});
            expect(flat.connect<"out", "in">(source, scale).has_value());
            expect(flat.connect<"out", "in">(scale, sink).has_value());

            gr::scheduler::Simple<> scheduler;
            expect(scheduler.exchange(std::move(flat)).has_value());
            expect(scheduler.runAndWait().has_value());
        }

        gr::Graph nested;
        std::ignore = nested.emplaceBlock<RampSource>({{"name", std::string("source")}, {"n_samples", 512U}});
        std::ignore = nested.emplaceBlock<RecordingSink>({{"name", std::string("rt-nested")}});

        auto  wrapper  = std::make_shared<GraphWrapper<gr::Graph>>();
        auto& interior = *wrapper->graph();
        auto& scale    = interior.emplaceBlock<Scale>({{"name", std::string("scale")}, {"gain", 2.0f}});

        const std::shared_ptr<BlockModel>& subgraph = nested.addBlock(wrapper);
        subgraph->setName("inner");
        expect(wrapper->exportPort(true, std::string(scale.unique_name), PortDirection::INPUT, "in", "in").has_value());
        expect(wrapper->exportPort(true, std::string(scale.unique_name), PortDirection::OUTPUT, "out", "out").has_value());
        expect(nested.connect(nested.blocks()[0], PortDefinition{"out"}, subgraph, PortDefinition{"in"}).has_value());
        expect(nested.connect(subgraph, PortDefinition{"out"}, nested.blocks()[1], PortDefinition{"in"}).has_value());

        auto loaded = gr::loadGrc(loader, gr::saveGrc(loader, nested));

        gr::scheduler::Simple<> scheduler;
        expect(scheduler.exchange(std::move(loaded)).has_value());
        expect(scheduler.runAndWait().has_value());

        const std::vector<float> flatSamples   = takeCollected("rt-flat");
        const std::vector<float> nestedSamples = takeCollected("rt-nested");
        expect(eq(flatSamples.size(), 512UZ));
        expect(flatSamples == nestedSamples) << "the round-tripped subgraph changed the stream";
    };

    "a loaded graph and its subgraphs bind the loader that loaded them"_test = [] {
        registerTestBlocks();

        BlockRegistry     localRegistry;
        SchedulerRegistry localSchedulers;
        expect(localRegistry.insert<RampSource>("=qa::RampSource") && localRegistry.insert<Scale>("=qa::Scale"));
        PluginLoader localLoader(localRegistry, localSchedulers, {});
        expect(&localLoader != &gr::globalPluginLoader());

        gr::Graph nested;
        std::ignore  = nested.emplaceBlock<RampSource>({{"name", std::string("source")}, {"n_samples", 16U}});
        auto wrapper = std::make_shared<GraphWrapper<gr::Graph>>();
        std::ignore  = wrapper->graph()->emplaceBlock<Scale>({{"name", std::string("scale")}});
        nested.addBlock(wrapper)->setName("inner");

        auto loaded = gr::loadGrc(localLoader, gr::saveGrc(localLoader, nested));
        expect(loaded->_pluginLoader == &localLoader) << "the root graph bound a loader other than the one that loaded it";

        bool sawSubgraph = false;
        for (const auto& block : loaded->blocks()) {
            if (gr::Graph* interior = block->graph(); interior != nullptr) {
                sawSubgraph = true;
                expect(interior->_pluginLoader == &localLoader) << "a loaded subgraph bound a loader other than the one that loaded it";
            }
        }
        expect(sawSubgraph) << "the loaded graph carries no subgraph, so nothing was checked";
    };
};

int main() { /* tests are run by the ut suite */ }
