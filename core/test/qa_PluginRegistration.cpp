#include <boost/ut.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <numeric>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>

#include "MappedLibrary.hpp"
#include "build_configure.hpp"
#include "plugins/good_registry_plugin.hpp"
#include "plugins/opaque_stage.hpp"

namespace qa_plugin_registration {

[[nodiscard]] std::string pluginDirectory() { return std::string(TESTS_BINARY_PATH) + "/plugins"; }

// one plugin at the ABI version this core implements, one at version 1, and one whose load registers a scheduler at version 1
[[nodiscard]] std::string abiPluginDirectory() { return std::string(TESTS_BINARY_PATH) + "/plugin_abi"; }

// the path of the plugin file in directory whose name contains name, in the form the loader opens it
[[nodiscard]] std::string pluginFile(std::string_view name, const std::string& directory = pluginDirectory()) {
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator{std::filesystem::path(directory)}) {
        if (entry.path().filename().string().contains(name)) {
            return entry.path().string();
        }
    }
    return {};
}

using gr::testing::isMapped;

// Opens GoodRegistry for the rest of the run, ahead of any loader. glibc keeps a shared object mapped until exit once
// it provides the first copy of an STB_GNU_UNIQUE symbol, and the order of a directory's loads decides which object
// does. GoodRegistry and GoodBase define the same such symbols. GoodRegistry then provides them, and GoodBase unmaps
// when it is closed.
[[nodiscard]] bool openUniqueSymbolProvider() {
    static void* const handle = gr::detail::openSharedLibrary(pluginFile("GoodRegistry").c_str());
    return handle != nullptr;
}

// Opens the plugin of this ABI version for the rest of the run, ahead of any loader, for the same reason: every such
// symbol of the two plugins the loader refuses is one this plugin defines too.
[[nodiscard]] bool openAbiUniqueSymbolProvider() {
    static void* const handle = gr::detail::openSharedLibrary(pluginFile("abi_probe_plugin.", abiPluginDirectory()).c_str());
    return handle != nullptr;
}

// the shared object that registers its own stages
[[nodiscard]] std::string opaqueStageDirectory() { return std::string(TESTS_BINARY_PATH) + "/opaque_stage_library"; }

template<typename T>
struct QueueSource : gr::Block<QueueSource<T>> {
    gr::PortOut<T> out;

    GR_MAKE_REFLECTABLE(QueueSource, out);

    [[nodiscard]] constexpr T processOne() const noexcept { return T{}; }
};

template<typename T>
struct QueueSink : gr::Block<QueueSink<T>> {
    gr::PortIn<T> in;

    GR_MAKE_REFLECTABLE(QueueSink, in);

    void processOne(T) {}
};

// a block of the program with the ports of the plugin's good::identity<float32>
struct NativeIdentity : gr::Block<NativeIdentity> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(NativeIdentity, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

using TaggedIndex = std::pair<std::size_t, std::string>;

// publishes the samples first, first + 1, ... with one tag at each sample index of tags
void publishCounting(gr::PortOut<float>& port, std::size_t first, std::size_t nSamples, std::span<const TaggedIndex> tags) {
    auto span = port.tryReserve<gr::SpanReleasePolicy::ProcessAll>(nSamples);
    for (const auto& [index, id] : tags) {
        span.publishTag(gr::property_map{{"id", id}}, index - first);
    }
    std::iota(span.begin(), span.end(), static_cast<float>(first));
    span.publish(nSamples);
}

// takes a block from the state its graph left it in to RUNNING. A case can then call its work()
void startForWork(gr::BlockModel& block) {
    if (block.state() == gr::lifecycle::State::IDLE) {
        std::ignore = block.changeStateTo(gr::lifecycle::State::INITIALISED);
    }
    std::ignore = block.changeStateTo(gr::lifecycle::State::RUNNING);
}

// calls work() until the sink's input holds nSamples samples, at most twice per sample
void workUntil(gr::BlockModel& stage, QueueSink<float>& sink, std::size_t nSamples) {
    for (std::size_t call = 0UZ; call < 2UZ * nSamples && sink.in.streamReader().available() < nSamples; ++call) {
        std::ignore = stage.work(std::numeric_limits<std::size_t>::max());
    }
}

// The graph holds a source, the stage and a sink, in this order. The stage passes the first samples and tags to its
// output, and the next ones stay queued at its input. The block of replacementType replaces the stage and passes on
// both. The sink is not called. Its input holds every sample in order and every tag at its sample index.
void expectQueuedSamplesAndTagsPassReplacement(gr::Graph& flow, QueueSource<float>& source, QueueSink<float>& sink, std::string_view replacementType) {
    using namespace boost::ut;
    constexpr std::size_t          kAtOutput = 6UZ;
    constexpr std::size_t          kAtInput  = 5UZ;
    const std::vector<TaggedIndex> kOutputTags{{0UZ, "first"}, {4UZ, "second"}};
    const std::vector<TaggedIndex> kInputTags{{kAtOutput + 1UZ, "third"}, {kAtOutput + 3UZ, "fourth"}};

    expect(fatal(flow.connect(flow.blocks()[0UZ], gr::PortDefinition("out"), flow.blocks()[1UZ], gr::PortDefinition("in")).has_value()));
    expect(fatal(flow.connect(flow.blocks()[1UZ], gr::PortDefinition("out"), flow.blocks()[2UZ], gr::PortDefinition("in")).has_value()));
    expect(fatal(flow.connectPendingEdges())) << "the graph did not connect";
    const std::shared_ptr<gr::BlockModel> stage = flow.blocks()[1UZ];

    publishCounting(source.out, 0UZ, kAtOutput, kOutputTags);
    startForWork(*stage);
    workUntil(*stage, sink, kAtOutput);
    expect(fatal(eq(sink.in.streamReader().available(), kAtOutput))) << "the stage did not pass the first samples to its output";
    publishCounting(source.out, kAtOutput, kAtInput, kInputTags);

    auto [replaced, replacement] = flow.replaceBlock(stage->uniqueName(), replacementType, {});
    expect(fatal(replacement != nullptr));
    const gr::BlockModel& replacedModel    = *replaced;
    const gr::BlockModel& replacementModel = *replacement;
    const std::type_info& replacedInfo     = typeid(replacedModel);
    const std::type_info& replacementInfo  = typeid(replacementModel);
    std::println("the replaced block's type_info is at {} ({}), the replacement's at {} ({})", static_cast<const void*>(&replacedInfo), replacedInfo.name(), static_cast<const void*>(&replacementInfo), replacementInfo.name());
    replaced.reset();
    startForWork(*replacement);
    workUntil(*replacement, sink, kAtOutput + kAtInput);

    const auto         received = sink.in.streamReader().get();
    std::vector<float> expected(kAtOutput + kAtInput);
    std::iota(expected.begin(), expected.end(), 0.f);
    expect(std::ranges::equal(received, expected)) << std::format("the sink's input holds {}, not {}", std::vector<float>(received.begin(), received.end()), expected);

    std::vector<TaggedIndex> published;
    for (const gr::Tag& sinkTag : sink.in.tagReader().get()) {
        if (const auto id = sinkTag.map.find("id"); id != sinkTag.map.end()) {
            published.emplace_back(sinkTag.index, id->second.value_or(std::string()));
        }
    }
    std::vector<TaggedIndex> expectedTags = kOutputTags;
    expectedTags.insert(expectedTags.end(), kInputTags.begin(), kInputTags.end());
    expect(std::ranges::equal(published, expectedTags)) << std::format("the sink's input holds the tags {}, not {}", published, expectedTags);
}

// The program's OpaqueStage<T> sits between a source and a sink. The shared object's stage of stageType has ports of the
// shared object's own T, and its replacement of the program's stage is refused at a port.
template<typename T>
void expectSharedObjectStageRefused(std::string_view stageType) {
    using namespace boost::ut;
    const std::vector<std::string> directories{opaqueStageDirectory()};
    gr::PluginLoader               loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);
    expect(fatal(loader.isBlockAvailable(stageType))) << "the shared object registered no block " << stageType;

    gr::Graph flow(loader);
    auto&     source = flow.emplaceBlock<QueueSource<T>>();
    auto&     stage  = flow.emplaceBlock<gr::testing::OpaqueStage<T>>();
    auto&     sink   = flow.emplaceBlock<QueueSink<T>>();
    expect(fatal(flow.connect<"out", "in">(source, stage).has_value()));
    expect(fatal(flow.connect<"out", "in">(stage, sink).has_value()));
    expect(fatal(flow.connectPendingEdges())) << "the graph did not connect";

    std::string refusal;
    try {
        std::ignore = flow.replaceBlock(stage.unique_name, stageType, {});
    } catch (const gr::exception& e) {
        refusal = e.what();
    }
    expect(refusal.contains("cannot take the buffers of port")) << "the replacement was not refused at a port, got: " << refusal;
    expect(flow.blocks()[1UZ]->uniqueName() == stage.unique_name) << "the refused replacement changed the graph";
}

} // namespace qa_plugin_registration

const boost::ut::suite<"PluginRegistration"> pluginRegistrationTests = [] {
    using namespace boost::ut;
    using namespace qa_plugin_registration;

    // A plugin's code can run after its loader is gone: in a block or a scheduler that the loader created, and in a
    // pool or a provider that the plugin registered when it loaded. This case comes first: the cases after it leave
    // the plugin mapped.
    "a plugin that has created nothing stays mapped after its loader is gone"_test = [] {
        const std::string file = pluginFile("GoodBasePlugin");
        expect(fatal(!file.empty())) << "the plugin file was not found";
        expect(fatal(openUniqueSymbolProvider())) << "the plugin that provides the shared symbols did not open";
        expect(fatal(!isMapped(file))) << "the plugin is mapped before its first load";
        {
            gr::BlockRegistry              registry;
            gr::SchedulerRegistry          schedulerRegistry;
            const std::vector<std::string> pluginDirectories{pluginDirectory()};
            gr::PluginLoader               loader(registry, schedulerRegistry, pluginDirectories);
            expect(fatal(loader.isBlockAvailable("good::fixed_source<float64>"))) << "the plugin registered no block";
            expect(fatal(isMapped(file))) << "the loaded plugin is not mapped";
        }
        expect(isMapped(file)) << "the loader unmapped a plugin that has created nothing";
    };

    "a plugin registers into the instance its own header declares"_test = [] {
        gr::BlockRegistry              registry;
        gr::SchedulerRegistry          schedulerRegistry;
        const std::vector<std::string> pluginDirectories{std::string(TESTS_BINARY_PATH) + "/plugins"};
        gr::PluginLoader               loader(registry, schedulerRegistry, pluginDirectories);

        for (const auto& [file, reason] : loader.failedPlugins()) {
            expect(file.find("Good") == std::string::npos) << std::format("{} failed to load: {}", file, reason);
        }

        const std::vector<std::string> available = loader.availableBlocks();
        for (const std::string_view name : {"good::identity<float32>", "good::identity<float64>", "good::cout_sink<float32>", "good::fixed_source<float64>"}) {
            expect(std::ranges::find(available, name) != available.end()) << std::format("{} is not provided by any loaded plugin", name);
        }
        expect(loader.instantiate("good::identity<float32>") != nullptr);
    };

    // The first case leaves the plugin mapped, and this case passes whenever that one does. It separates only two
    // loaders that fail the first case: one that keeps a plugin once it has created a block, and one that keeps none.
    "a plugin that has created a block stays mapped after its loader is gone"_test = [] {
        const std::string file = pluginFile("GoodBasePlugin");
        expect(fatal(!file.empty())) << "the plugin file was not found";
        expect(fatal(openUniqueSymbolProvider())) << "the plugin that provides the shared symbols did not open";
        {
            gr::BlockRegistry              registry;
            gr::SchedulerRegistry          schedulerRegistry;
            const std::vector<std::string> pluginDirectories{pluginDirectory()};
            gr::PluginLoader               loader(registry, schedulerRegistry, pluginDirectories);
            expect(fatal(loader.instantiate("good::fixed_source<float64>") != nullptr)) << "the plugin created no block";
            expect(fatal(isMapped(file))) << "the loaded plugin is not mapped";
        }
        expect(isMapped(file)) << "the loader unmapped a plugin that has created a block";
    };

    // A plugin's code runs when it loads, before the loader checks its ABI version and the versions of the schedulers its
    // load registered. A pool or a provider registered then can point into it after the loader refuses it.
    "a plugin refused after its code ran stays mapped after its loader is gone"_test = [] {
        const std::string earlier = pluginFile("abi_probe_plugin_v1", abiPluginDirectory());
        const std::string foreign = pluginFile("abi_probe_plugin_foreign_scheduler", abiPluginDirectory());
        expect(fatal(!earlier.empty() && !foreign.empty())) << "the plugin files were not found";
        expect(fatal(openAbiUniqueSymbolProvider())) << "the plugin that provides the shared symbols did not open";
        expect(fatal(!isMapped(earlier) && !isMapped(foreign))) << "a refused plugin is mapped before its first load";
        {
            gr::BlockRegistry              registry;
            gr::SchedulerRegistry          schedulerRegistry;
            const std::vector<std::string> pluginDirectories{abiPluginDirectory()};
            gr::PluginLoader               loader(registry, schedulerRegistry, pluginDirectories);
            expect(fatal(loader.failedPlugins().contains(earlier))) << "the plugin of ABI version 1 was not refused";
            expect(fatal(loader.failedPlugins().contains(foreign))) << "the plugin that registered a scheduler of ABI version 1 was not refused";
            expect(eq(loader.plugins().size(), 1UZ)) << "the loader admitted a refused plugin";
            expect(that % loader.blockLibraries().empty()) << "the loader took a refused plugin as a shared object of blocks";
        }
        expect(isMapped(earlier)) << "the loader unmapped a plugin it refused for its ABI version";
        expect(isMapped(foreign)) << "the loader unmapped a plugin it refused for the version of a scheduler it registered";
    };

    // The program and the plugin each compile good::identity<float32> and its port types.
    "a plugin block replaces a native block of its type and passes on the samples and tags queued at its ports"_test = [] {
        constexpr std::string_view     kPluginIdentity = "good::identity<float32>";
        gr::BlockRegistry              registry;
        gr::SchedulerRegistry          schedulerRegistry;
        const std::vector<std::string> pluginDirectories{pluginDirectory()};
        gr::PluginLoader               loader(registry, schedulerRegistry, pluginDirectories);
        expect(fatal(loader.isBlockAvailable(kPluginIdentity) && !registry.contains(kPluginIdentity))) << "the plugin is not the only provider of the block";

        gr::Graph flow(loader);
        auto&     source = flow.emplaceBlock<QueueSource<float>>();
        std::ignore      = flow.emplaceBlock<good::identity<float>>();
        auto& sink       = flow.emplaceBlock<QueueSink<float>>();
        expectQueuedSamplesAndTagsPassReplacement(flow, source, sink, kPluginIdentity);
    };

    "a native block replaces a plugin block and passes on the samples and tags queued at its ports"_test = [] {
        gr::BlockRegistry              registry;
        gr::SchedulerRegistry          schedulerRegistry;
        const std::vector<std::string> pluginDirectories{pluginDirectory()};
        gr::PluginLoader               loader(registry, schedulerRegistry, pluginDirectories);
        expect(fatal(registry.insert<NativeIdentity>()));

        gr::Graph flow(loader);
        auto&     source = flow.emplaceBlock<QueueSource<float>>();
        expect(fatal(flow.emplaceBlock("good::identity<float32>", {}) != nullptr)) << "the plugin created no block";
        auto& sink = flow.emplaceBlock<QueueSink<float>>();
        expectQueuedSamplesAndTagsPassReplacement(flow, source, sink, gr::meta::type_name<NativeIdentity>());
    };

    "a plugin block whose ports carry another value type cannot replace a native block"_test = [] {
        gr::BlockRegistry              registry;
        gr::SchedulerRegistry          schedulerRegistry;
        const std::vector<std::string> pluginDirectories{pluginDirectory()};
        gr::PluginLoader               loader(registry, schedulerRegistry, pluginDirectories);

        gr::Graph flow(loader);
        auto&     source = flow.emplaceBlock<QueueSource<float>>();
        auto&     stage  = flow.emplaceBlock<good::identity<float>>();
        expect(fatal(flow.connect<"out", "in">(source, stage).has_value()));
        expect(fatal(flow.connectPendingEdges())) << "the graph did not connect";

        std::string refusal;
        try {
            std::ignore = flow.replaceBlock(stage.unique_name, "good::identity<float64>", {});
        } catch (const gr::exception& e) {
            refusal = e.what();
        }
        expect(refusal.contains("cannot take the buffers of port in")) << "the replacement was not refused at its input, got: " << refusal;
        expect(flow.blocks()[1UZ]->uniqueName() == stage.unique_name) << "the refused replacement changed the graph";
    };

    // The shared object's stages and the program's carry value types of one name that are distinct types.
    "a block of a shared object whose port value type is in an anonymous namespace cannot replace the program's block of the same name"_test = [] { expectSharedObjectStageRefused<gr::testing::OpaqueSample>("test::opaque_stage"); };

    "a block of a shared object whose port value type is a local class of a static function cannot replace the program's block of the same name"_test = [] { expectSharedObjectStageRefused<gr::testing::LocalSample>("test::local_stage"); };
};

int main() { /* not needed for UT */ }
