#include <boost/ut.hpp>

#include <gnuradio-4.0/BlockModel.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

#include "settings_utils.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
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

    struct Settings {
        Annotated<gr::Size_t, Doc<"samples to emit before finishing, or 0 to run until stopped">> n_samples = 4096U;

        GR_MAKE_REFLECTABLE(Settings, n_samples);
    } settings;

    GR_MAKE_REFLECTABLE(RampSource, out, settings);

    gr::Size_t emitted = 0U;

    explicit RampSource(property_map init = {}) : Block<RampSource>(std::move(init)) {}

    work::Status processBulk(OutputSpanLike auto& outSpan) {
        if (settings.n_samples > 0U && emitted >= settings.n_samples) {
            outSpan.publish(0UZ);
            return work::Status::DONE;
        }
        const std::size_t room = settings.n_samples > 0U ? std::min(outSpan.size(), static_cast<std::size_t>(settings.n_samples - emitted)) : outSpan.size();
        if (room == 0UZ) {
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        for (std::size_t i = 0UZ; i < room; ++i) {
            outSpan[i] = static_cast<float>(emitted + static_cast<gr::Size_t>(i));
        }
        emitted += static_cast<gr::Size_t>(room);
        outSpan.publish(room);
        return work::Status::OK;
    }
};

struct Scale : Block<Scale> {
    PortIn<float>  in;
    PortOut<float> out;

    struct Settings {
        Annotated<float, Doc<"output = gain * input">>        gain    = 1.0f;
        Annotated<std::string>                                label   = "scale";
        Annotated<bool>                                       enabled = true;
        Annotated<std::vector<float>, Doc<"carried, unused">> taps    = std::vector<float>{1.0f};

        GR_MAKE_REFLECTABLE(Settings, gain, label, enabled, taps);
    } settings;

    GR_MAKE_REFLECTABLE(Scale, in, out, settings);

    explicit Scale(property_map init = {}) : Block<Scale>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return settings.enabled ? value * settings.gain : value; }
};

/// a multi-input block: its inputs are one collection port, addressed "in#0" and "in#1"
struct SumInputs : Block<SumInputs> {
    std::vector<PortIn<float>> in;
    PortOut<float>             out;

    struct Settings {
        Annotated<gr::Size_t, Limits<Min<1U>, Max<8U>>, PortCountFor<"in">> n_inputs = 2U;

        GR_MAKE_REFLECTABLE(Settings, n_inputs);
    } settings;

    GR_MAKE_REFLECTABLE(SumInputs, in, out, settings);

    explicit SumInputs(property_map init = {}) : Block<SumInputs>(std::move(init)) { in.resize(settings.n_inputs); }

    void applySettings(const Settings& s) { in.resize(s.n_inputs); }

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

/// scales by `gain`
struct FrontEnd : Block<FrontEnd> {
    PortIn<float>  in;
    PortOut<float> out;

    struct Settings {
        Annotated<float> gain = 1.0f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;

    GR_MAKE_REFLECTABLE(FrontEnd, in, out, settings);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * settings.gain; }
};

/// a block whose settings come from a description: one output per channel, each channel with a frequency that has a
/// default and an amplifier gain that has none
struct ChannelRadio : Block<ChannelRadio> {
    PortIn<float>               in;
    std::vector<PortOut<float>> outputs{2UZ};

    GR_MAKE_REFLECTABLE(ChannelRadio, in, outputs);

    property_map describedSettings;

    static property_map settingsDescription() {
        return property_map{
            {"version", 1},
            {"block_type", "qa::ChannelRadio"},
            {"settings", property_map{{"n_channels", property_map{{"type", "uint32"}, {"default", 2}, {"port_count_for", "outputs"}}}, {"sample_rate", property_map{{"type", "float64"}, {"default", 10.0e6}}}}},
            {"groups", property_map{{"channel", property_map{{"count", "n_channels"}, {"settings", property_map{{"frequency", property_map{{"type", "float64"}, {"default", 100.0e6}}}, {"lna", property_map{{"type", "float64"}, {"minimum", 0.0}, {"maximum", 40.0}}}}}}}}},
        };
    }

    gr::work::Status processBulk(auto&, auto&) { return gr::work::Status::OK; }
};

/// a block that reads a setting in its constructor, as a block sizing itself from its construction argument does
struct ConstructionRecorder : Block<ConstructionRecorder> {
    PortIn<float>  in;
    PortOut<float> out;

    struct Settings {
        Annotated<float> gain = 1.0f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;

    GR_MAKE_REFLECTABLE(ConstructionRecorder, in, out, settings);

    std::optional<float> gainAtConstruction; // the gain in the constructor's argument, if it held one

    // a graph file's untagged real number is a double, a caller's setting here a float
    explicit ConstructionRecorder(property_map init = {}) : Block<ConstructionRecorder>(init) {
        if (const auto it = init.find("gain"); it != init.end()) {
            if (const float* given = it->second.get_if<float>(); given != nullptr) {
                gainAtConstruction = *given;
            } else if (const double* fromFile = it->second.get_if<double>(); fromFile != nullptr) {
                gainAtConstruction = static_cast<float>(*fromFile);
            }
        }
    }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * settings.gain; }
};

inline void registerTestBlocks() {
    static const bool registered = [] {
        BlockRegistry& registry = globalBlockRegistry();
        return registry.insert<RampSource>("=qa::RampSource") && registry.insert<Scale>("=qa::Scale")                  //
               && registry.insert<SumInputs>("=qa::SumInputs") && registry.insert<RecordingSink>("=qa::RecordingSink") //
               && registry.insert<ConstructionRecorder>("=qa::ConstructionRecorder") && registry.insert<FrontEnd>("=qa::FrontEnd");
    }();
    expect(registered) << "the test blocks must reach the global registry";
}

/// a scheduler that keeps the map its constructor receives
struct ConstructionRecordingScheduler : gr::scheduler::Simple<> {
    property_map constructedWith; // the constructor's argument

    explicit ConstructionRecordingScheduler(property_map init = {}) : gr::scheduler::Simple<>(init), constructedWith(std::move(init)) {}
};

/// a scheduler a graph file can name for a managed subgraph
inline void registerTestScheduler() {
    static const bool registered = gr::globalSchedulerRegistry().insert<ConstructionRecordingScheduler>("=qa::Simple");
    expect(registered) << "the test scheduler must reach the global registry";
}

inline BlockModel& onlyBlock(const gr::Graph& graph) {
    expect(boost::ut::fatal(eq(graph.blocks().size(), 1UZ)));
    return *graph.blocks().front();
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

/**
 * A definitions root carrying one recipe, written to a temporary directory.
 *
 * A recipe reaches a graph file only through a PluginLoader's definition roots, and its interior
 * gain is derived from `gain_factor`, which is declared without a default and is therefore
 * required: the composite cannot be built until the value is known.
 */
struct RecipeAssetRoot {
    std::filesystem::path path = std::filesystem::temp_directory_path() / "gr4_qa_grc_recipe";

    RecipeAssetRoot() {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        std::ofstream index(path / "index.yaml");
        index << "assets:\n  - file: gain_recipe.yaml\n    created: \"2024-01-01-00:00:00\"\n    modified: \"2024-01-15-10:00:00\"\n    block_type: qa::GainRecipe\n";
        std::ofstream asset(path / "gain_recipe.yaml");
        asset << R"yaml(definition_metadata:
  block_type: qa::GainRecipe
blocks:
  - id: SUBGRAPH
    parameters:
      name: gain_recipe
    exported_parameters:
      - name: gain_factor
        type: float32
    graph:
      blocks:
        - id: "qa::Scale"
          parameters:
            name: inner
            gain: "=gain_factor"
      exported_ports:
        - [inner, INPUT, in, in]
        - [inner, OUTPUT, out, out]
)yaml";
    }

    RecipeAssetRoot(const RecipeAssetRoot&)            = delete;
    RecipeAssetRoot& operator=(const RecipeAssetRoot&) = delete;

    ~RecipeAssetRoot() { std::filesystem::remove_all(path); }
};

/// a loader over its own registry, so the recipe's interior id resolves without the global one
inline gr::PluginLoader recipeLoader(const std::vector<std::string>& roots) {
    static BlockRegistry     registry;
    static SchedulerRegistry schedulers;
    static const bool        registered = registry.insert<Scale>("=qa::Scale");
    boost::ut::expect(registered) << "the interior block must reach the recipe loader's registry";
    return PluginLoader(registry, schedulers, roots);
}

/// the unique names of the graph's blocks in insertion order, comma separated
inline std::string blockNames(const gr::Graph& graph) {
    std::string names;
    for (const std::shared_ptr<BlockModel>& block : graph.blocks()) {
        names += std::format("{}{}", names.empty() ? "" : ", ", block->uniqueName());
    }
    return names;
}

/// `disconnect_on_done` as the block's settings read it, true when it is missing
inline bool disconnectsOnDone(const BlockModel& block) { return block.get<bool>("disconnect_on_done").value_or(true); }

/// the message of the gr::exception `call` throws, empty when it throws none
inline std::string refusalOf(const std::function<void()>& call) {
    try {
        call();
    } catch (const gr::exception& refusal) {
        return refusal.message;
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

        const property_map parameters = gr::settings::heldValues(loaded->blocks().front()->get());
        expect(parameters.at("gain") == pmt::Value(2.5f)) << "a float setting did not survive";
        expect(parameters.at("label") == pmt::Value(std::string("carried"))) << "a string setting did not survive";
        expect(parameters.at("enabled") == pmt::Value(false)) << "a bool setting did not survive";
    };

    "a block's tag couplings survive the round trip, and the loaded block follows its key"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        gr::Graph          flow;
        auto&              scale = flow.emplaceBlock<Scale>({{"name", std::string("scale")}, {"gain", 2.5f}});
        const property_map couplings{{"gain", std::string("foo:gain")}};
        expect(scale.setTagCouplings(couplings).has_value());

        const std::string dump = gr::saveGrc(loader, flow);
        expect(dump.contains("tag_couplings")) << "the saved graph holds no tag_couplings: " << dump;
        auto loaded = gr::loadGrc(loader, dump);
        expect(fatal(eq(loaded->blocks().size(), 1UZ)));
        gr::SettingsBase& settings = gr::settings::detail::settingsOf(*loaded->blocks().front());
        expect(settings.tagCouplings() == couplings) << "the table does not survive the round trip";

        const float taggedGain = 4.0f;
        std::ignore            = gr::settings::detail::FrameworkAccess::tagEnters(settings, property_map{{"foo:gain", taggedGain}});
        std::ignore            = gr::testing::applyStagedForTest(settings);
        expect(settings.get<float>("gain") == taggedGain) << "the gain the file sets does not follow the key the file couples it to";
    };

    "a graph file that couples a setting to a key the framework acts on is refused, naming the block"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        const std::string document = R"yaml(blocks:
  - id: qa::Scale
    parameters:
      name: scale
    tag_couplings:
      gain: "gr:reset_default"
)yaml";

        std::string reported;
        try {
            const auto loaded = gr::loadGrc(loader, document);
            expect(false) << std::format("{} blocks loaded; a coupling to gr:reset_default must be refused", loaded->blocks().size());
        } catch (const gr::exception& e) {
            reported = e.message;
        }
        expect(reported.contains("'scale'") && reported.contains("qa::Scale")) << reported;
        expect(reported.contains("gain") && reported.contains("gr:reset_default")) << reported;
    };

    // a list without a type tag reaches the block as a sequence of type-erased values
    "an untagged list in a graph file sets a vector setting"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        const std::string document = R"yaml(blocks:
  - id: qa::Scale
    parameters:
      name: scale
      taps: [0.5, 2, 0.25]
)yaml";

        std::string        reported;
        std::vector<float> applied;
        try {
            auto loaded = gr::loadGrc(loader, document);
            expect(eq(loaded->blocks().size(), 1UZ));
            applied = static_cast<Scale*>(loaded->blocks().front()->raw())->settings.taps.value;
        } catch (const gr::exception& e) {
            reported = e.message;
        }
        expect(reported.empty()) << reported;
        expect(eq(applied.size(), 3UZ)) << "the block holds the list";
        if (applied.size() == 3UZ) {
            expect(eq(applied[0], 0.5f));
            expect(eq(applied[1], 2.0f));
            expect(eq(applied[2], 0.25f));
        }
    };

    "a graph file's list and single number take not-a-number and infinity"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        const std::string  listDocument = R"yaml(blocks:
  - id: qa::Scale
    parameters:
      name: scale
      taps: [.nan, -.inf, 1]
)yaml";
        std::string        reported;
        std::vector<float> applied;
        try {
            auto loaded = gr::loadGrc(loader, listDocument);
            applied     = static_cast<Scale*>(loaded->blocks().front()->raw())->settings.taps.value;
        } catch (const gr::exception& e) {
            reported = e.message;
        }
        expect(reported.empty()) << reported;
        expect(fatal(eq(applied.size(), 3UZ)));
        expect(std::isnan(applied[0]) && std::isinf(applied[1]) && applied[1] < 0.f);

        const std::string scalarDocument = R"yaml(blocks:
  - id: qa::Scale
    parameters:
      name: scale
      gain: .nan
)yaml";
        reported.clear();
        float gain = 0.f;
        try {
            const auto loaded = gr::loadGrc(loader, scalarDocument);
            gain              = static_cast<Scale*>(loaded->blocks().front()->raw())->settings.gain.value;
        } catch (const gr::exception& e) {
            reported = e.message;
        }
        expect(reported.empty()) << reported;
        expect(std::isnan(gain));
    };

    "an untagged list element that does not convert refuses the graph file"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        const std::string document = R"yaml(blocks:
  - id: qa::Scale
    parameters:
      name: scale
      taps: [0.5, two]
)yaml";

        std::string reported;
        try {
            const auto loaded = gr::loadGrc(loader, document);
            expect(false) << std::format("{} blocks loaded; a list element that does not convert must be refused", loaded->blocks().size());
        } catch (const gr::exception& e) {
            reported = e.message;
        }
        expect(reported.contains("taps: element 1: not a decimal number")) << reported;
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
        expect(fatal(subgraph->setName("inner").has_value()));
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
        expect(scale.set({{"gain", 9.0f}}, gr::settings::noWait).has_value()) << "the staged value must be accepted";
        expect(gr::settings::detail::FrameworkAccess::hasWork(gr::settings::detail::settingsOf(scale))) << "the value must be staged, not applied";

        const std::string dump = gr::saveGrc(loader, flow);

        expect(gr::settings::detail::FrameworkAccess::hasWork(gr::settings::detail::settingsOf(scale))) << "saveGrc committed the staged parameters";
        expect(eq(scale.settings.gain.value, 3.0f)) << "saveGrc changed the graph it saved";

        auto loaded = gr::loadGrc(loader, dump);
        expect(eq(loaded->blocks().size(), 1UZ));
        expect(gr::settings::heldValues(loaded->blocks().front()->get()).at("gain") == pmt::Value(9.0f)) << "the dump must carry what the block would use";
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
        expect(fatal(subgraph->setName("inner").has_value()));
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
        expect(fatal(nested.addBlock(wrapper)->setName("inner").has_value()));

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

    "a subgraph whose graph field is not a map is refused"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        const std::string document = R"yaml(blocks:
  - id: SUBGRAPH
    parameters:
      name: group
    graph: 42
)yaml";

        std::string reported;
        try {
            const auto loaded = gr::loadGrc(loader, document);
            expect(false) << std::format("{} blocks loaded; a subgraph whose graph field is a scalar must be refused", loaded->blocks().size());
        } catch (const gr::exception& e) {
            reported = e.message;
        }
        expect(reported.contains("group")) << reported;
        expect(reported.contains("SUBGRAPH")) << reported;
        expect(reported.contains("graph")) << reported;
    };

    // the same shape in the rest of the reader: each of these fields carries a report for a value
    // of the wrong type, and the non-terminating read is what lets that report be emitted
    "a malformed field elsewhere in the document is refused"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        const std::vector<std::pair<std::string, std::string>> documents{
            {"parameters is not a map", R"yaml(blocks:
  - id: qa::Scale
    parameters: 42
)yaml"},
            {"a scheduler is not a map", R"yaml(blocks:
  - id: SUBGRAPH
    parameters:
      name: group
    scheduler: 42
    graph:
      blocks: []
)yaml"},
            {"an exported_ports entry is not a list", R"yaml(blocks:
  - id: SUBGRAPH
    parameters:
      name: group
    graph:
      blocks:
        - id: qa::Scale
          parameters:
            name: inner
      exported_ports:
        - 42
)yaml"},
            {"a connections entry is not a list", R"yaml(blocks:
  - id: qa::Scale
    parameters:
      name: scale
connections:
  - 42
)yaml"},
            {"a port index is neither a name nor a number", R"yaml(blocks:
  - id: qa::RampSource
    parameters:
      name: src
  - id: qa::Scale
    parameters:
      name: scale
connections:
  - [src, {}, scale, in]
)yaml"},
        };

        for (const auto& [what, document] : documents) {
            bool refused = false;
            try {
                const auto loaded = gr::loadGrc(loader, document);
                expect(false) << std::format("{}: {} blocks loaded, the file must be refused", what, loaded->blocks().size());
            } catch (const gr::exception&) {
                refused = true;
            }
            expect(refused) << what;
        }
    };

    "a blocks entry that is not a map is passed over"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        const auto loaded = gr::loadGrc(loader, "blocks:\n  - 42\n");
        expect(eq(loaded->blocks().size(), 0UZ)) << "an entry that is not a block must be passed over, not fatal";
    };

    // the reader asks getProperty for a std::string everywhere today, which takes the other branch
    // of its if constexpr; the non-string branch is exercised directly so that its incorrect-type
    // report is pinned for the first T that needs it
    "getProperty reports a non-string field of the wrong type"_test = [] {
        const gr::property_map object{{"count", std::string("many")}};

        const auto read = gr::detail::getProperty<std::int64_t>(object, "count");
        expect(!read.has_value()) << "a string where an integer belongs must not be read as one";
        if (!read.has_value()) {
            expect(read.error().message.contains("count")) << read.error().message;
        }
    };

    // a recipe derives its interior from the exported parameters. The reader therefore supplies them
    // at instantiation rather than staging them after the block exists
    "a graph file names a recipe and supplies its required parameter"_test = [] {
        const RecipeAssetRoot assets;
        auto                  loader = recipeLoader({assets.path.string()});

        const std::string document = R"yaml(blocks:
  - id: qa::GainRecipe
    parameters:
      name: demod
      gain_factor: !!float32 3.5
)yaml";

        auto loaded = gr::loadGrc(loader, document);
        expect(eq(loaded->blocks().size(), 1UZ));
        const std::shared_ptr<BlockModel>& composite = loaded->blocks().front();
        expect(composite->graph() != nullptr) << "the recipe must come back as a composite";
        if (composite->graph() == nullptr || composite->graph()->blocks().empty()) {
            expect(false) << "the composite carries no interior block";
            return;
        }

        expect(eq(std::string(composite->name()), std::string("demod"))) << "the file's own name did not reach the block";
        expect(!composite->metaInformation().contains("gain_factor")) << "a recipe parameter must not be left behind as meta_information";

        const auto gain = composite->graph()->blocks().front()->get<float>("gain");
        expect(gain.has_value());
        expect(eq(gain.value_or(float{}), 3.5f)) << "the supplied parameter did not reach the interior setting";
    };

    "a recipe's parameter record round-trips through a saved graph"_test = [] {
        const RecipeAssetRoot assets;
        auto                  loader = recipeLoader({assets.path.string()});

        gr::Graph                          graph(loader);
        const std::shared_ptr<BlockModel>& composite = graph.emplaceBlock("qa::GainRecipe", {{"name", std::string("demod")}, {"gain_factor", 3.5f}});
        expect(gr::settings::detail::FrameworkAccess::isGiven(gr::settings::detail::settingsOf(*composite), "gain_factor")) << "the parameter the recipe is built with is given";
        expect(eq(composite->typeName(), std::string_view("gr::Graph"))) << "a message to its graph finds a graph";
        expect(eq(composite->recipeType(), std::string_view("qa::GainRecipe")));
        expect(fatal(composite->set(gr::property_map{{"gain_factor", 4.5f}}, gr::settings::noWait).has_value()));
        expect(fatal(composite->setTagCouplings(gr::property_map{{"gain_factor", std::string("qa:gain")}}).has_value()));

        const std::string saved = gr::saveGrc(loader, graph);
        expect(saved.contains("gain_factor") && saved.contains("qa:gain")) << saved;
        auto loaded = gr::loadGrc(loader, saved);
        expect(fatal(eq(loaded->blocks().size(), 1UZ)));
        const std::shared_ptr<BlockModel>& reloaded = loaded->blocks().front();
        expect(gr::settings::detail::FrameworkAccess::isGiven(gr::settings::detail::settingsOf(*reloaded), "gain_factor")) << "the loaded composite's record holds the parameter";
        expect(eq(reloaded->get<float>("gain_factor").value_or(0.f), 4.5f));
        const gr::property_map couplings = reloaded->tagCouplings();
        expect(couplings.contains("gain_factor") && couplings.at("gain_factor") == pmt::Value(std::string("qa:gain"))) << "the parameter's coupling came back";
        expect(fatal(reloaded->graph() != nullptr && !reloaded->graph()->blocks().empty()));
        expect(eq(reloaded->graph()->blocks().front()->get<float>("gain").value_or(0.f), 4.5f)) << "the load derives the inner gain from the saved parameter";
    };

    "a recipe composite attached without a recipe type saves as a subgraph and loads with its inner graph"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        gr::Graph flow;
        auto      wrapper  = std::make_shared<GraphWrapper<gr::Graph>>();
        auto&     interior = *wrapper->graph();
        std::ignore        = interior.emplaceBlock<Scale>({{"name", std::string("inner-scale")}, {"gain", 1.0f}});
        std::vector<gr::recipe::ParameterDeclaration> declarations{{.name = "boost", .type = "float32", .defaultValue = pmt::Value(1.0f), .doc = ""}};
        std::vector<gr::recipe::Binding>              bindings{{.namePath = {"inner-scale"}, .settingKey = "gain", .expression = {}, .substituted = 0UZ, .sequence = {}}};
        expect(fatal(wrapper->attachRecipeBindings({.declarations = std::move(declarations), .bindings = std::move(bindings), .values = {pmt::Value(1.0f)}}, gr::property_map{{"boost", 1.0f}}).has_value()));
        expect(fatal(wrapper->set(gr::property_map{{"boost", 3.0f}}, gr::settings::noWait).has_value()));
        expect(fatal(wrapper->setTagCouplings(gr::property_map{{"boost", std::string("qa:boost")}}).has_value()));
        std::ignore = flow.addBlock(wrapper);

        const std::string saved = gr::saveGrc(loader, flow);
        expect(saved.contains("SUBGRAPH") && !saved.contains("boost")) << saved;
        auto loaded = gr::loadGrc(loader, saved);
        expect(fatal(eq(loaded->blocks().size(), 1UZ)));
        const std::shared_ptr<BlockModel>& reloaded = loaded->blocks().front();
        expect(fatal(reloaded->graph() != nullptr && eq(reloaded->graph()->blocks().size(), 1UZ))) << "the inner graph came back";
        expect(eq(reloaded->graph()->blocks().front()->get<float>("gain").value_or(0.f), 3.0f)) << "the inner block keeps the value the parameter derived";
    };

    "a graph file that omits a recipe's required parameter names the block and the parameter"_test = [] {
        const RecipeAssetRoot assets;
        auto                  loader = recipeLoader({assets.path.string()});

        const std::string document = R"yaml(blocks:
  - id: qa::GainRecipe
    parameters:
      name: demod
)yaml";

        std::string reported;
        try {
            const auto loaded = gr::loadGrc(loader, document);
            expect(loaded->blocks().empty()) << "a recipe without its required parameter must not load";
        } catch (const gr::exception& e) {
            reported = e.message;
        }
        expect(reported.contains("demod")) << reported;
        expect(reported.contains("qa::GainRecipe")) << reported;
        expect(reported.contains("gain_factor")) << reported;
    };

    // emplaceBlock hands a recipe its exported parameters and applies the caller's `name` as the label
    "emplaceBlock names a recipe and supplies its required parameter"_test = [] {
        const RecipeAssetRoot assets;
        auto                  loader = recipeLoader({assets.path.string()});

        gr::Graph                          graph(loader);
        const std::shared_ptr<BlockModel>& composite = graph.emplaceBlock("qa::GainRecipe", {{"name", std::string("demod")}, {"gain_factor", 3.5f}});

        expect(composite->graph() != nullptr) << "the recipe must come back as a composite";
        if (composite->graph() == nullptr || composite->graph()->blocks().empty()) {
            expect(false) << "the composite carries no interior block";
            return;
        }

        expect(eq(std::string(composite->name()), std::string("demod"))) << "the caller's name did not reach the block";
        expect(!composite->metaInformation().contains("gain_factor")) << "a recipe parameter must not be left behind as meta_information";

        const auto gain = composite->graph()->blocks().front()->get<float>("gain");
        expect(gain.has_value());
        expect(eq(gain.value_or(float{}), 3.5f)) << "the supplied parameter did not reach the interior setting";
    };

    "emplaceBlock without a recipe's required parameter names the block and the parameter"_test = [] {
        const RecipeAssetRoot assets;
        auto                  loader = recipeLoader({assets.path.string()});

        gr::Graph   graph(loader);
        std::string reported;
        try {
            graph.emplaceBlock("qa::GainRecipe", {{"name", std::string("demod")}});
            expect(false) << "a recipe without its required parameter must not be placed";
        } catch (const gr::exception& e) {
            reported = e.message;
        }
        expect(reported.contains("demod")) << reported;
        expect(reported.contains("qa::GainRecipe")) << reported;
        expect(reported.contains("gain_factor")) << reported;
    };

    // replaceBlock builds the recipe as emplaceBlock does
    "replaceBlock puts a recipe in the place of a block and supplies its required parameter"_test = [] {
        const RecipeAssetRoot assets;
        auto                  loader = recipeLoader({assets.path.string()});

        gr::Graph         graph(loader);
        const std::string replacedName(graph.emplaceBlock("qa::Scale", {{"name", std::string("scale")}, {"gain", 2.0f}})->uniqueName());

        const auto [oldBlock, composite] = graph.replaceBlock(replacedName, "qa::GainRecipe", {{"name", std::string("demod")}, {"gain_factor", 3.5f}});

        expect(composite->graph() != nullptr) << "the recipe must come back as a composite";
        if (composite->graph() == nullptr || composite->graph()->blocks().empty()) {
            expect(false) << "the composite carries no interior block";
            return;
        }

        expect(eq(std::string(composite->name()), std::string("demod"))) << "the caller's name did not reach the block";
        expect(!composite->metaInformation().contains("gain_factor")) << "a recipe parameter must not be left behind as meta_information";

        const auto gain = composite->graph()->blocks().front()->get<float>("gain");
        expect(gain.has_value());
        expect(eq(gain.value_or(float{}), 3.5f)) << "the supplied parameter did not reach the interior setting";
    };

    "replaceBlock without a recipe's required parameter names the block and the parameter"_test = [] {
        const RecipeAssetRoot assets;
        auto                  loader = recipeLoader({assets.path.string()});

        gr::Graph         graph(loader);
        const std::string replacedName(graph.emplaceBlock("qa::Scale", {{"name", std::string("scale")}})->uniqueName());

        std::string reported;
        try {
            std::ignore = graph.replaceBlock(replacedName, "qa::GainRecipe", {{"name", std::string("demod")}});
            expect(false) << "a recipe without its required parameter must not replace a block";
        } catch (const gr::exception& e) {
            reported = e.message;
        }
        expect(reported.contains("demod")) << reported;
        expect(reported.contains("qa::GainRecipe")) << reported;
        expect(reported.contains("gain_factor")) << reported;
    };

    // the composite takes the settings its definition does not export before it joins the graph, so a refused value
    // leaves the graph with the blocks it held
    "emplaceBlock with a setting a recipe refuses leaves the recipe out of the graph"_test = [] {
        const RecipeAssetRoot assets;
        auto                  loader = recipeLoader({assets.path.string()});
        gr::Graph             graph(loader);
        std::ignore                     = graph.emplaceBlock("qa::Scale", {{"name", std::string("before")}});
        const std::size_t nBlocksBefore = graph.blocks().size();
        const std::string namesBefore   = blockNames(graph);

        const std::string refused = refusalOf([&] { std::ignore = graph.emplaceBlock("qa::GainRecipe", {{"name", std::string("demod")}, {"gain_factor", 3.5f}, {"compute_domain", 7.0f}}); });
        expect(refused.contains("'demod'") && refused.contains("qa::GainRecipe") && refused.contains("compute_domain")) << "the refusal names the block, its type and the key: " << refused;
        expect(eq(graph.blocks().size(), nBlocksBefore)) << "the refused recipe joined the graph";
        expect(eq(blockNames(graph), namesBefore));

        const std::shared_ptr<BlockModel>& accepted = graph.emplaceBlock("qa::GainRecipe", {{"name", std::string("demod")}, {"gain_factor", 3.5f}, {"disconnect_on_done", false}});
        expect(eq(graph.blocks().size(), nBlocksBefore + 1UZ)) << "a recipe with settings it takes joins the graph";
        expect(!disconnectsOnDone(*accepted)) << "a setting the recipe does not export is in force once the recipe is in the graph";
    };

    "replaceBlock with a setting a recipe refuses leaves the replaced block in place"_test = [] {
        const RecipeAssetRoot assets;
        auto                  loader = recipeLoader({assets.path.string()});
        gr::Graph             graph(loader);
        std::ignore = graph.emplaceBlock("qa::Scale", {{"name", std::string("before")}});
        const std::string replacedName(graph.emplaceBlock("qa::Scale", {{"name", std::string("scale")}})->uniqueName());
        const std::size_t nBlocksBefore = graph.blocks().size();
        const std::string namesBefore   = blockNames(graph);

        const std::string refused = refusalOf([&] { std::ignore = graph.replaceBlock(replacedName, "qa::GainRecipe", {{"name", std::string("demod")}, {"gain_factor", 3.5f}, {"compute_domain", 7.0f}}); });
        expect(refused.contains("'demod'") && refused.contains("qa::GainRecipe") && refused.contains("compute_domain")) << "the refusal names the block, its type and the key: " << refused;
        expect(eq(graph.blocks().size(), nBlocksBefore));
        expect(eq(blockNames(graph), namesBefore)) << "the block to be replaced left the graph";

        const auto [oldBlock, composite] = graph.replaceBlock(replacedName, "qa::GainRecipe", {{"name", std::string("demod")}, {"gain_factor", 3.5f}, {"disconnect_on_done", false}});
        expect(eq(std::string(oldBlock->uniqueName()), replacedName)) << "a recipe with settings it takes replaces the block";
        expect(eq(graph.blocks().size(), nBlocksBefore));
        expect(!disconnectsOnDone(*composite)) << "a setting the recipe does not export is in force once the recipe is in the graph";
    };

    "a graph file with a setting a recipe refuses leaves the recipe out of the graph"_test = [] {
        const RecipeAssetRoot assets;
        auto                  loader = recipeLoader({assets.path.string()});
        gr::Graph             graph(loader);
        std::ignore                     = graph.emplaceBlock("qa::Scale", {{"name", std::string("before")}});
        const std::size_t nBlocksBefore = graph.blocks().size();
        const std::string namesBefore   = blockNames(graph);

        const auto document = [](std::string_view setting) {
            const auto parsed = gr::pmt::yaml::deserialize(std::format("blocks:\n  - id: qa::GainRecipe\n    parameters:\n      name: demod\n      gain_factor: !!float32 3.5\n      {}\n", setting));
            expect(parsed.has_value()) << "the fixture document must parse";
            return parsed.has_value() ? *parsed : gr::property_map{};
        };
        const std::string refused = refusalOf([&] { std::ignore = gr::detail::loadGraphFromMap(loader, graph, document("compute_domain: 7")); });
        expect(refused.contains("'demod'") && refused.contains("qa::GainRecipe") && refused.contains("compute_domain")) << "the refusal names the block, its type and the key: " << refused;
        expect(eq(graph.blocks().size(), nBlocksBefore)) << "the refused recipe joined the graph";
        expect(eq(blockNames(graph), namesBefore));

        const std::string accepted = refusalOf([&] { std::ignore = gr::detail::loadGraphFromMap(loader, graph, document("disconnect_on_done: false")); });
        expect(accepted.empty()) << "a recipe with settings it takes loads: " << accepted;
        expect(fatal(graph.blocks().size() == nBlocksBefore + 1UZ)) << "and joins the graph";
        expect(!disconnectsOnDone(*graph.blocks().back())) << "a setting the recipe does not export is in force once the recipe is in the graph";
    };

    // the two connections name the one input by name and by index
    "a graph file with two connections into one stream input is refused"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        const std::string document = R"yaml(blocks:
  - id: qa::RampSource
    parameters:
      name: first
  - id: qa::RampSource
    parameters:
      name: second
  - id: qa::RecordingSink
    parameters:
      name: joined
connections:
  - [first, out, joined, in]
  - [second, out, joined, 0]
)yaml";

        std::string reported;
        try {
            const auto loaded = gr::loadGrc(loader, document);
            expect(false) << std::format("{} edges loaded; a stream input with two sources must be refused", loaded->edges().size());
        } catch (const gr::exception& e) {
            reported = e.message;
        }
        expect(reported.contains("joined/")) << reported;
        expect(reported.contains("first/out")) << reported;
        expect(reported.contains("second/out")) << reported;
    };

    "a saved graph carries the listed edge and not the refused second edge into its input"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        gr::Graph flow;
        auto&     first  = flow.emplaceBlock<RampSource>({{"name", std::string("first")}, {"n_samples", 64U}});
        auto&     second = flow.emplaceBlock<RampSource>({{"name", std::string("second")}, {"n_samples", 64U}});
        auto&     sink   = flow.emplaceBlock<RecordingSink>({{"name", std::string("replaced-edge-sink")}});
        expect(flow.connect<"out", "in">(first, sink).has_value());
        expect(!flow.connect<"out", "in">(second, sink).has_value()) << "a second edge into the input was accepted";

        const auto saved = pmt::yaml::deserialize(gr::saveGrc(loader, flow));
        expect(fatal(saved.has_value())) << "a dump this writer produced must parse";
        const auto* connections = saved->at("connections").get_if<Tensor<pmt::Value>>();
        expect(fatal(connections != nullptr)) << "the saved graph has no connections list";
        expect(fatal(eq(connections->size(), 1UZ))) << "the saved graph keeps the refused edge";
        const auto* connection = (*connections)[0].get_if<Tensor<pmt::Value>>();
        expect(fatal(connection != nullptr && connection->size() >= 4UZ));
        expect(eq((*connection)[0].value_or(std::string_view{}), std::string_view(first.unique_name))) << "the saved edge is not the listed one";

        const auto loaded = gr::loadGrc(loader, gr::saveGrc(loader, flow));
        expect(eq(loaded->edges().size(), 1UZ));
    };
};

/**
 * `loadGrc` with a caller's settings. Each block reads them with the file's parameters. A name or a key the graph
 * cannot take is refused before the graph is returned.
 */
const boost::ut::suite<"GRC load with settings"> grcSettingsTests = [] {
    using namespace boost::ut;
    using namespace gr;
    using namespace qa_grc;

    static constexpr std::string_view kSummedRamps = R"yaml(blocks:
  - id: qa::RampSource
    parameters:
      name: left
      n_samples: 64
  - id: qa::RampSource
    parameters:
      name: middle
      n_samples: 64
  - id: qa::RampSource
    parameters:
      name: right
      n_samples: 64
  - id: qa::SumInputs
    parameters:
      name: sum
      n_inputs: 2
  - id: qa::RecordingSink
    parameters:
      name: summed-sink
connections:
  - [left, out, sum, [0, 0]]
  - [middle, out, sum, [0, 1]]
  - [right, out, sum, [0, 2]]
  - [sum, out, summed-sink, in]
)yaml";

    "a given n_inputs sizes the collection before the connections are made"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        auto loaded = gr::loadGrc(loader, kSummedRamps, gr::BlockSettings{{"sum", {{"n_inputs", gr::Size_t{3U}}}}});
        expect(eq(loaded->edges().size(), 4UZ)) << "the third input exists when the file connects it";

        gr::scheduler::Simple<> scheduler;
        expect(scheduler.exchange(std::move(loaded)).has_value());
        expect(scheduler.runAndWait().has_value());
        const std::vector<float> samples = takeCollected("summed-sink");
        expect(eq(samples.size(), 64UZ));
        expect(eq(samples.back(), 3.0f * 63.0f)) << "all three ramps reach the sum";
    };

    // the edges are resolved when the scheduler takes the graph, so the file's own count shows at the run
    "the file alone does not bring the third ramp to the sum"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        auto                    loaded = gr::loadGrc(loader, kSummedRamps);
        gr::scheduler::Simple<> scheduler;
        bool                    summedThree = false;
        if (scheduler.exchange(std::move(loaded)).has_value() && scheduler.runAndWait().has_value()) {
            const std::vector<float> samples = takeCollected("summed-sink");
            summedThree                      = !samples.empty() && samples.back() == 3.0f * 63.0f;
        }
        std::ignore = takeCollected("summed-sink");
        expect(!summedThree) << "the file's own n_inputs of 2 leaves in#2 absent";
    };

    "a given value replaces the file's value and leaves the other keys"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        const auto             loaded   = gr::loadGrc(loader, R"yaml(blocks:
  - id: qa::Scale
    parameters:
      name: scale
      gain: 3.0
      label: from-the-file
)yaml",
                          gr::BlockSettings{{"scale", {{"gain", 5.0f}}}});
        const gr::property_map settings = gr::settings::heldValues(loaded->blocks().front()->get());
        expect(settings.at("gain") == pmt::Value(5.0f));
        expect(settings.at("label") == pmt::Value(std::string("from-the-file")));
    };

    "a name no block carries and a key a block does not declare are refused with the nearest names"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        auto reportOf = [&loader](const gr::BlockSettings& overrides) {
            try {
                std::ignore = gr::loadGrc(loader, kSummedRamps, overrides);
            } catch (const gr::exception& e) {
                return e.message;
            }
            return std::string{};
        };

        const std::string unknownBlock = reportOf({{"summ", {{"n_inputs", gr::Size_t{3U}}}}});
        expect(unknownBlock.contains("block 'summ'")) << unknownBlock;
        expect(unknownBlock.contains("'sum'")) << "the nearest name is offered" << unknownBlock;

        const std::string unknownKey = reportOf({{"sum", {{"n_input", gr::Size_t{3U}}}}});
        expect(unknownKey.contains("declares no setting named 'n_input'")) << unknownKey;
        expect(unknownKey.contains("'n_inputs'")) << "the nearest key is offered" << unknownKey;
    };

    "a name two blocks share is refused"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        std::string reported;
        try {
            std::ignore = gr::loadGrc(loader, R"yaml(blocks:
  - id: qa::Scale
    parameters:
      name: twin
  - id: qa::Scale
    parameters:
      name: twin
)yaml",
                gr::BlockSettings{{"twin", {{"gain", 2.0f}}}});
        } catch (const gr::exception& e) {
            reported = e.message;
        }
        expect(reported.contains("settings are given for block 'twin', and 2 blocks carry that unique_name or name")) << reported;
    };

    "a key that is one block's unique_name and another's name is refused, as are two keys for one block"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        auto reportOf = [&loader](std::string_view document, const gr::BlockSettings& overrides) {
            try {
                std::ignore = gr::loadGrc(loader, document, overrides);
            } catch (const gr::exception& e) {
                return e.message;
            }
            return std::string{};
        };

        const std::string collision = reportOf(R"yaml(blocks:
  - id: qa::Scale
    unique_name: target
    parameters:
      name: first
  - id: qa::Scale
    parameters:
      name: target
)yaml",
            {{"target", {{"gain", 9.0f}}}});
        expect(collision.contains("block 'target', and 2 blocks carry")) << collision;
        expect(collision.contains("block 0 'first' of type 'qa::Scale' with unique_name 'target'")) << collision;
        expect(collision.contains("block 1 'target' of type 'qa::Scale'")) << collision;

        const std::string twice = reportOf(R"yaml(blocks:
  - id: qa::Scale
    unique_name: only
    parameters:
      name: scale
)yaml",
            {{"only", {{"gain", 2.0f}}}, {"scale", {{"gain", 3.0f}}}});
        expect(twice.contains("settings are given twice for block 0 'scale'")) << twice;
    };

    "a key that is a block's unique_name reaches that block alone, beside another of the same name"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        const auto loaded = gr::loadGrc(loader, R"yaml(blocks:
  - id: qa::Scale
    unique_name: left-scale
    parameters:
      name: scale
      gain: 3.0
  - id: qa::Scale
    parameters:
      name: scale
      gain: 3.0
)yaml",
            gr::BlockSettings{{"left-scale", {{"gain", 5.0f}}}});
        expect(eq(loaded->blocks().size(), 2UZ));
        if (loaded->blocks().size() == 2UZ) {
            expect(eq(static_cast<Scale*>(loaded->blocks()[0]->raw())->settings.gain.value, 5.0f)) << "the block of that unique_name takes the entry";
            expect(eq(static_cast<Scale*>(loaded->blocks()[1]->raw())->settings.gain.value, 3.0f)) << "the other block keeps the file's value";
        }
    };

    "every block is constructed with its parameters, merged where it has an entry"_test = [] {
        registerTestBlocks();
        PluginLoader& loader = gr::globalPluginLoader();

        auto recorderOf = [](const std::shared_ptr<BlockModel>& block) -> ConstructionRecorder& { return *static_cast<ConstructionRecorder*>(block->raw()); };

        const std::shared_ptr<BlockModel> direct = loader.instantiate("qa::ConstructionRecorder", {{"gain", 4.0f}});
        expect(direct != nullptr);
        if (direct != nullptr) {
            expect(recorderOf(direct).gainAtConstruction == std::optional(4.0f)) << "the registry hands its argument to the constructor";
        }

        const auto loaded = gr::loadGrc(loader, R"yaml(blocks:
  - id: qa::ConstructionRecorder
    meta_information:
      unread: a value the block does not declare
    parameters:
      name: given
      gain: 3.0
  - id: qa::ConstructionRecorder
    meta_information:
      unread: a value the block does not declare
    parameters:
      name: from-file
      gain: 3.0
)yaml",
            gr::BlockSettings{{"given", {{"gain", 5.0f}}}});
        expect(eq(loaded->blocks().size(), 2UZ));
        if (loaded->blocks().size() == 2UZ) {
            const std::shared_ptr<BlockModel>& given    = loaded->blocks()[0];
            const std::shared_ptr<BlockModel>& fromFile = loaded->blocks()[1];
            expect(recorderOf(given).gainAtConstruction == std::optional(5.0f)) << "the constructor reads the given value";
            expect(eq(recorderOf(given).settings.gain.value, 5.0f)) << "the settings hold the given value";
            expect(given->metaInformation().contains("unread")) << "the entry's meta_information reaches the block";
            expect(recorderOf(fromFile).gainAtConstruction == std::optional(3.0f)) << "a block without an entry is constructed with the file's parameters";
            expect(eq(recorderOf(fromFile).settings.gain.value, 3.0f));
            expect(fromFile->metaInformation().contains("unread")) << "the entry's meta_information reaches the block";
        }

        const auto alone = gr::loadGrc(loader, R"yaml(blocks:
  - id: qa::ConstructionRecorder
    parameters:
      name: alone
      gain: 3.0
)yaml");
        expect(eq(alone->blocks().size(), 1UZ));
        if (alone->blocks().size() == 1UZ) {
            expect(recorderOf(alone->blocks().front()).gainAtConstruction == std::optional(3.0f)) << "a load without settings constructs the block with the file's parameters";
        }
    };

    // the location parameter follows the settings. A call that passes a location third does not compile.
    "loadGrc compiles with two arguments, with settings, and with settings and a location"_test = [] {
        static_assert(requires(PluginLoader& loader) { gr::loadGrc(loader, std::string_view{}); });
        static_assert(requires(PluginLoader& loader) { gr::loadGrc(loader, std::string_view{}, gr::BlockSettings{}); });
        static_assert(requires(PluginLoader& loader) { gr::loadGrc(loader, std::string_view{}, {}); });
        static_assert(requires(PluginLoader& loader) { gr::loadGrc(loader, std::string_view{}, gr::BlockSettings{}, std::source_location::current()); });
        static_assert(requires(PluginLoader& loader) { gr::loadGrc(loader, std::string_view{}, {}, std::source_location::current()); });

        registerTestBlocks();
        const auto loaded = gr::loadGrc(gr::globalPluginLoader(), R"yaml(blocks:
  - id: qa::Scale
    parameters:
      name: scale
)yaml",
            {});
        expect(eq(loaded->blocks().size(), 1UZ));
    };

    "a graph with two member keys set saves them flat with no ungiven key, and its load gives the same record"_test = [] {
        BlockRegistry     localRegistry;
        SchedulerRegistry localSchedulers;
        expect(fatal(localRegistry.insert<ChannelRadio>("=qa::ChannelRadio")));
        PluginLoader loader(localRegistry, localSchedulers, {});

        gr::Graph flow;
        auto&     radio = flow.emplaceBlock<ChannelRadio>({{"name", std::string("radio")}});
        expect(fatal(radio.set(property_map{{"channel.0.frequency", 145.0e6}, {"channel.1.lna", 12.0}}).has_value()));

        const std::string text     = gr::saveGrc(loader, flow);
        const auto        document = pmt::yaml::deserialize(text);
        expect(fatal(document.has_value())) << text;
        const auto* blocks = document->at("blocks").get_if<Tensor<pmt::Value>>();
        expect(fatal(blocks != nullptr && blocks->size() == 1UZ)) << text;
        const auto* parameters = (*blocks)[0].get_if<property_map>()->at("parameters").get_if<property_map>();
        expect(fatal(parameters != nullptr)) << text;
        expect(parameters->contains("channel.0.frequency") && parameters->contains("channel.1.lna")) << text;
        for (std::string_view ungiven : {"channel", "channel.0.lna", "channel.1.frequency", "n_channels", "sample_rate"}) {
            expect(!parameters->contains(ungiven)) << std::format("the file holds '{}':\n{}", ungiven, text);
        }

        const auto loaded = gr::loadGrc(loader, text);
        expect(fatal(loaded->blocks().size() == 1UZ));
        const std::shared_ptr<BlockModel>& again      = loaded->blocks()[0];
        const auto                         recordText = [](const SettingsBase& settings) {
            const std::expected<gr::settings::Values, gr::settings::Rejected> record = settings.get();
            std::string                                                       entries;
            for (const auto& [key, entry] : record.value_or(gr::settings::Values{})) {
                if (key == "unique_name") { // each instance numbers its own
                    continue;
                }
                entries += std::format("{}={}:{} ", key, static_cast<int>(entry.state), entry.value);
            }
            return record.has_value() ? entries : std::string("refused");
        };
        expect(eq(recordText(gr::settings::detail::settingsOf(*again)), recordText(gr::settings::detail::settingsOf(radio)))) << "the loaded record differs from the saved one";
        expect(gr::settings::detail::FrameworkAccess::savedValues(gr::settings::detail::settingsOf(*again)) == gr::settings::detail::FrameworkAccess::savedValues(gr::settings::detail::settingsOf(radio))) << "the loaded block's given keys differ from the saved block's";
    };
};

/**
 * A subgraph entry's `parameters` reach the subgraph's own settings as a block's reach the block's,
 * from the file, through the writer and from a caller's settings.
 */
const boost::ut::suite<"GRC subgraph settings"> grcSubgraphSettingsTests = [] {
    using namespace boost::ut;
    using namespace gr;
    using namespace qa_grc;

    static constexpr std::string_view kGroup = R"yaml(blocks:
  - id: SUBGRAPH
    unique_name: the-group
    meta_information:
      unread: a value the subgraph does not declare
    parameters:
      name: group
      disconnect_on_done: false
    graph:
      blocks:
        - id: qa::Scale
          parameters:
            name: inner
            gain: 2.0
)yaml";

    static constexpr std::string_view kManaged = R"yaml(blocks:
  - id: SUBGRAPH
    parameters:
      name: managed
      timeout_ms: 5
      watchdog_timeout: 300
    scheduler:
      id: qa::Simple
      parameters:
        name: from-the-scheduler
        timeout_ms: 7
    graph:
      blocks:
        - id: qa::Scale
          parameters:
            name: inner
)yaml";

    "a subgraph's parameters load into its settings"_test = [] {
        registerTestBlocks();
        const auto  loaded = gr::loadGrc(gr::globalPluginLoader(), kGroup);
        BlockModel& group  = onlyBlock(*loaded);

        expect(eq(std::string(group.name()), std::string("group")));
        expect(gr::settings::heldValues(group.get()).at("disconnect_on_done") == pmt::Value(false)) << "the file's value must replace the default";
        expect(group.metaInformation().contains("unread")) << "the entry's meta_information reaches the subgraph";
        expect(fatal(group.graph() != nullptr));
        expect(eq(group.graph()->blocks().size(), 1UZ)) << "the interior loads beside the settings";
    };

    "a managed subgraph takes its scheduler's parameters over its own and keeps its own name"_test = [] {
        registerTestBlocks();
        registerTestScheduler();
        const auto         loaded     = gr::loadGrc(gr::globalPluginLoader(), kManaged);
        BlockModel&        managed    = onlyBlock(*loaded);
        const property_map parameters = gr::settings::heldValues(managed.get());

        expect(eq(std::string(managed.name()), std::string("managed")));
        expect(parameters.at("timeout_ms") == pmt::Value(gr::Size_t{7U})) << "the scheduler's value wins";
        expect(parameters.at("watchdog_timeout") == pmt::Value(gr::Size_t{300U})) << "a key the scheduler leaves out keeps the entry's value";

        const property_map& constructedWith = static_cast<ConstructionRecordingScheduler*>(managed.raw())->constructedWith;
        expect(constructedWith.contains("watchdog_timeout")) << "the scheduler is constructed with the entry's parameters";
        expect(constructedWith.contains("timeout_ms") && constructedWith.at("timeout_ms") == pmt::Value(std::int64_t{7})) << "the scheduler is constructed with its own parameters over the entry's";
        expect(constructedWith.contains("name") && constructedWith.at("name").value_or(std::string_view{}) == "managed") << "the scheduler is constructed with the entry's name";
    };

    "a subgraph's settings survive the round trip"_test = [] {
        registerTestBlocks();
        registerTestScheduler();
        PluginLoader& loader = gr::globalPluginLoader();

        for (const std::string_view document : {kGroup, kManaged}) {
            const auto loaded            = gr::loadGrc(loader, document);
            const auto [before, after]   = roundTrip(loader, *loaded);
            const std::string difference = describeDifference(before, after);
            expect(difference.empty()) << difference;
        }

        const auto  group = gr::loadGrc(loader, gr::saveGrc(loader, *gr::loadGrc(loader, kGroup)));
        BlockModel& saved = onlyBlock(*group);
        expect(gr::settings::heldValues(saved.get()).at("disconnect_on_done") == pmt::Value(false)) << "the writer must save the subgraph's settings";

        const auto  managed      = gr::loadGrc(loader, gr::saveGrc(loader, *gr::loadGrc(loader, kManaged)));
        BlockModel& savedManaged = onlyBlock(*managed);
        expect(eq(std::string(savedManaged.name()), std::string("managed")));
        expect(gr::settings::heldValues(savedManaged.get()).at("timeout_ms") == pmt::Value(gr::Size_t{7U})) << "the writer must save the scheduler's settings";
    };

    "a caller's settings reach a subgraph by its unique_name or its name"_test = [] {
        registerTestBlocks();
        registerTestScheduler();
        PluginLoader& loader = gr::globalPluginLoader();

        for (const std::string_view key : {"the-group", "group"}) {
            const auto  loaded = gr::loadGrc(loader, kGroup, gr::BlockSettings{{std::string(key), {{"disconnect_on_done", true}}}});
            BlockModel& group  = onlyBlock(*loaded);
            expect(gr::settings::heldValues(group.get()).at("disconnect_on_done") == pmt::Value(true)) << key;
            expect(eq(std::string(group.name()), std::string("group"))) << key;
        }

        const auto  loaded  = gr::loadGrc(loader, kManaged, gr::BlockSettings{{"managed", {{"timeout_ms", gr::Size_t{9U}}}}});
        BlockModel& managed = onlyBlock(*loaded);
        expect(gr::settings::heldValues(managed.get()).at("timeout_ms") == pmt::Value(gr::Size_t{9U})) << "the caller's value wins over the scheduler's";
        expect(gr::settings::heldValues(managed.get()).at("watchdog_timeout") == pmt::Value(gr::Size_t{300U}));
    };

    "a key the subgraph does not declare is refused with the nearest names"_test = [] {
        registerTestBlocks();
        registerTestScheduler();

        std::string reported;
        try {
            std::ignore = gr::loadGrc(gr::globalPluginLoader(), kGroup, gr::BlockSettings{{"group", {{"disconnect_on_don", true}}}});
        } catch (const gr::exception& e) {
            reported = e.message;
        }
        expect(reported.contains("block 'group'")) << reported;
        expect(reported.contains("declares no setting named 'disconnect_on_don'")) << reported;
        expect(reported.contains("'disconnect_on_done'")) << "the nearest key is offered" << reported;

        std::string reportedManaged;
        try {
            std::ignore = gr::loadGrc(gr::globalPluginLoader(), kManaged, gr::BlockSettings{{"managed", {{"timeout_m", gr::Size_t{9U}}}}});
        } catch (const gr::exception& e) {
            reportedManaged = e.message;
        }
        expect(reportedManaged.contains("block 'managed'")) << reportedManaged;
        expect(reportedManaged.contains("declares no setting named 'timeout_m'")) << reportedManaged;
        expect(reportedManaged.contains("'timeout_ms'")) << "the nearest key is offered" << reportedManaged;
    };
};

/**
 * A graph file fails to load when a block refuses a key it does not declare or a value outside a setting's limits.
 */
const boost::ut::suite<"GRC load refusals"> grcRefusalTests = [] {
    using namespace boost::ut;
    using namespace gr;
    using namespace qa_grc;

    auto reportOf = [](std::string_view yaml) {
        try {
            std::ignore = gr::loadGrc(gr::globalPluginLoader(), yaml);
        } catch (const gr::exception& e) {
            return e.message;
        }
        return std::string{};
    };

    "a file value outside a setting's limits fails the load naming the block, the key and both bounds"_test = [&reportOf] {
        registerTestBlocks();
        const std::string report = reportOf(R"yaml(blocks:
  - id: qa::SumInputs
    parameters:
      name: sum
      n_inputs: 9
)yaml");
        expect(report.contains("block 'sum'")) << report;
        expect(report.contains("n_inputs: out of range [1, 8]: 9")) << report;
    };

    "a file value outside a setting's limits reaches the loader's caller as the block's refusal of its settings"_test = [] {
        registerTestBlocks();
        std::optional<gr::settings::Rejected> rejected;
        try {
            std::ignore = gr::loadGrc(gr::globalPluginLoader(), R"yaml(blocks:
  - id: qa::SumInputs
    parameters:
      name: sum
      n_inputs: 9
)yaml");
        } catch (const gr::settings::RefusedSettings& refused) {
            rejected = refused.rejected;
        } catch (const gr::exception&) {
        }
        expect(fatal(rejected.has_value())) << "the refusal reached the caller as a plain gr::exception";
        expect(*rejected == gr::settings::Rejected{{.key = "n_inputs", .reason = "out of range [1, 8]: 9"}}) << gr::settings::describeRejected(*rejected);
    };

    "a file's misspelled parameter fails the load naming the nearest key"_test = [&reportOf] {
        registerTestBlocks();
        const std::string report = reportOf(R"yaml(blocks:
  - id: qa::SumInputs
    parameters:
      name: sum
      n_input: 3
)yaml");
        expect(report.contains("block 'sum'")) << report;
        expect(report.contains("declares no setting named 'n_input'")) << report;
        expect(report.contains("'n_inputs'")) << "the nearest key is offered" << report;
    };

    "a file the writer made from every field it writes loads unchanged"_test = [] {
        registerTestBlocks();
        registerTestScheduler();
        PluginLoader& loader = gr::globalPluginLoader();

        // every field the writer writes, at every level: a block's unique_name, block_category, meta_information and
        // tag_couplings, a managed subgraph with its scheduler, a subgraph nested in it with its exported ports, and
        // a connection carrying its buffer size
        const auto loaded = gr::loadGrc(loader, R"yaml(blocks:
  - id: qa::RampSource
    unique_name: the-source
    block_category: NormalBlock
    meta_information:
      role: source
    parameters:
      name: source
      n_samples: 64
  - id: qa::Scale
    parameters:
      name: scale
      gain: 2.0
    tag_couplings:
      gain: qa:gain
  - id: SUBGRAPH
    meta_information:
      role: managed
    parameters:
      name: managed
    scheduler:
      id: qa::Simple
      parameters:
        timeout_ms: 7
    graph:
      blocks:
        - id: SUBGRAPH
          parameters:
            name: nested
          graph:
            blocks:
              - id: qa::Scale
                parameters:
                  name: inner
            exported_ports:
              - [inner, INPUT, in, in]
              - [inner, OUTPUT, out, out]
      exported_ports:
        - [nested, INPUT, in, in]
        - [nested, OUTPUT, out, out]
  - id: qa::RecordingSink
    parameters:
      name: sink
connections:
  - [the-source, out, scale, in, 4096]
  - [scale, out, managed, in]
  - [managed, out, sink, in]
)yaml");

        const std::string written = gr::saveGrc(loader, *loaded);
        for (const std::string_view field : {"unique_name:", "block_category:", "meta_information:", "tag_couplings:", "scheduler:", "exported_ports:", "4096"}) {
            expect(written.contains(field)) << std::format("the written file holds no '{}', so it pins nothing about that field: {}", field, written);
        }

        const auto [before, after]   = roundTrip(loader, *loaded);
        const std::string difference = describeDifference(before, after);
        expect(difference.empty()) << difference;
    };

    "meta_information holds keys no block declares and the format does not name"_test = [] {
        registerTestBlocks();
        const auto  loaded = gr::loadGrc(gr::globalPluginLoader(), R"yaml(blocks:
  - id: qa::Scale
    parameters:
      name: scale
    meta_information:
      layout_hint: top-left
      application_note: free text
      paramaters:
        nested: 3
)yaml");
        BlockModel& scale  = onlyBlock(*loaded);
        for (const std::string_view key : {"layout_hint", "application_note", "paramaters"}) {
            expect(scale.metaInformation().contains(std::string(key))) << std::format("meta_information lost '{}'", key);
        }
        expect(!gr::settings::heldValues(scale.get()).contains("layout_hint")) << "a key under meta_information is no setting";
    };
};
