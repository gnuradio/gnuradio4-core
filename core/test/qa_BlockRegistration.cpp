#include <boost/ut.hpp>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * The generated definition units register through gr::makeBlockRegistration() and
 * gr::insertBlockFactory() instead of gr::registerBlock(). Both paths must leave the registry in the
 * same state, key for key and alias for alias, or a block that a graph resolves by name today stops
 * resolving tomorrow.
 */

namespace qa_registration {

struct Sink : gr::Block<Sink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(Sink, in);

    explicit Sink(gr::property_map init = {}) : gr::Block<Sink>(std::move(init)) {}

    void processOne(float) const noexcept {}
};

struct DefaultPolicy {};

template<typename T, typename TPolicy = DefaultPolicy>
struct Scale : gr::Block<Scale<T, TPolicy>> {
    gr::PortIn<T>  in;
    gr::PortOut<T> out;

    GR_MAKE_REFLECTABLE(Scale, in, out);

    explicit Scale(gr::property_map init = {}) : gr::Block<Scale<T, TPolicy>>(std::move(init)) {}

    [[nodiscard]] constexpr T processOne(T value) const noexcept { return value; }
};

/// two revisions of one block under one name, the older one marked deprecated
struct Filter : gr::Block<Filter> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(Filter, in, out);

    explicit Filter(gr::property_map init = {}) : gr::Block<Filter>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

struct FilterV1 : gr::Block<FilterV1> {
    static constexpr gr::block::Status  status{.deprecated = true, .experimental = true};
    static constexpr gr::block::Version version = 1U;

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(FilterV1, in, out);

    explicit FilterV1(gr::property_map init = {}) : gr::Block<FilterV1>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

struct FilterV2 : gr::Block<FilterV2> {
    static constexpr gr::block::Version version = 2U;

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(FilterV2, in, out);

    explicit FilterV2(gr::property_map init = {}) : gr::Block<FilterV2>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

template<typename TBlock>
std::unique_ptr<gr::BlockModel> makeBlock(gr::property_map params) {
    return std::make_unique<gr::BlockWrapper<TBlock>>(std::move(params));
}

/// registers `TBlock` under `key`, keeping the version and status the type declares
template<typename TBlock>
bool insertAs(gr::BlockRegistry& registry, std::string_view key) {
    return registry.insert(key, "", &makeBlock<TBlock>, gr::block::versionOf<TBlock>(), gr::block::statusOf<TBlock>());
}

std::string joined(const std::vector<std::string>& keys) {
    std::string out;
    for (const std::string& key : keys) {
        out += key;
        out += '|';
    }
    return out;
}

template<typename TBlock, gr::meta::fixed_string OverrideName = "">
void expectRegistrationParity(std::string_view what) {
    using namespace boost::ut;

    gr::BlockRegistry typedRegistry;
    gr::registerBlock<TBlock, OverrideName>(typedRegistry);

    gr::BlockRegistry factoryRegistry;
    expect(gr::insertBlockFactory(factoryRegistry, gr::makeBlockRegistration<TBlock, OverrideName>(&makeBlock<TBlock>))) << what;

    const std::vector<std::string> typedKeys = typedRegistry.keys();
    expect(!typedKeys.empty()) << what;
    expect(eq(joined(typedKeys), joined(factoryRegistry.keys()))) << what;

    for (const std::string& key : typedKeys) {
        const std::shared_ptr<gr::BlockModel> fromTyped   = typedRegistry.create(key, {});
        const std::shared_ptr<gr::BlockModel> fromFactory = factoryRegistry.create(key, {});
        expect(fromTyped != nullptr) << key;
        expect(fromFactory != nullptr) << key;
        if (fromTyped == nullptr || fromFactory == nullptr) {
            continue;
        }
        expect(eq(std::string(fromTyped->typeName()), std::string(fromFactory->typeName()))) << key;
        expect(eq(typedRegistry.typeName(fromTyped), factoryRegistry.typeName(fromFactory))) << key << "alias";
    }
}

} // namespace qa_registration

const boost::ut::suite<"block registration"> blockRegistrationTests = [] {
    using namespace boost::ut;

    "a block with no template parameters"_test = [] { qa_registration::expectRegistrationParity<qa_registration::Sink>("Sink"); };

    "a template instantiation, whose alias carries the parameter spelling"_test = [] { qa_registration::expectRegistrationParity<qa_registration::Scale<float>>("Scale<float>"); };

    "a defaulted template parameter, which the key spells out and the alias need not"_test = [] { qa_registration::expectRegistrationParity<qa_registration::Scale<std::complex<float>>>("Scale<complex<float32>>"); };

    "an overridden name, which registers a second key"_test = [] {
        qa_registration::expectRegistrationParity<qa_registration::Sink, "qa::CustomSink">("Sink as qa::CustomSink");

        gr::BlockRegistry registry;
        expect(gr::insertBlockFactory(registry, gr::makeBlockRegistration<qa_registration::Sink, "qa::CustomSink">(&qa_registration::makeBlock<qa_registration::Sink>)));
        expect(eq(registry.keys().size(), 2UZ));
        expect(registry.contains("qa::CustomSink"));
        expect(registry.contains(gr::meta::type_name<qa_registration::Sink>()));
    };

    "insert reports a newly added alias"_test = [] {
        gr::BlockRegistry registry;
        expect(gr::insertBlockFactory(registry, gr::makeBlockRegistration<qa_registration::Sink, "">(&qa_registration::makeBlock<qa_registration::Sink>)));
        expect(gr::insertBlockFactory(registry, gr::makeBlockRegistration<qa_registration::Sink, "qa::CustomSink">(&qa_registration::makeBlock<qa_registration::Sink>))) //
            << "the alias key is new even though the name key repeats";
        expect(!gr::insertBlockFactory(registry, gr::makeBlockRegistration<qa_registration::Sink, "qa::CustomSink">(&qa_registration::makeBlock<qa_registration::Sink>))) //
            << "repeating both keys adds nothing";
    };

    "the factory a definition unit exports builds the block the key names"_test = [] {
        gr::BlockRegistry registry;
        expect(gr::insertBlockFactory(registry, gr::makeBlockRegistration<qa_registration::Scale<float>>(&qa_registration::makeBlock<qa_registration::Scale<float>>)));

        const std::string                     key   = gr::meta::type_name<qa_registration::Scale<float>>();
        const std::shared_ptr<gr::BlockModel> block = registry.create(key, {{"name", std::string("scaled")}});
        expect(block != nullptr) << key;
        if (block == nullptr) {
            return;
        }
        expect(eq(std::string(block->typeName()), key));

        // the key comes from the type, the alias from the reflected spelling, so a defaulted template
        // parameter appears in one and not the other -- both must resolve
        expect(eq(key, std::string("qa_registration::Scale<float32, qa_registration::DefaultPolicy>")));
        expect(registry.contains("qa_registration::Scale<float32>")) << "the reflected alias";
        expect(registry.create("qa_registration::Scale<float32>", {}) != nullptr) << "the reflected alias";
    };
};

const boost::ut::suite<"block status and version"> blockStatusTests = [] {
    using namespace boost::ut;
    using gr::block::Status;
    using gr::block::Version;

    "a block that declares neither is version 1 with no flag set"_test = [] {
        expect(eq(gr::block::versionOf<qa_registration::Filter>(), gr::block::kDefaultVersion));
        expect(gr::block::statusOf<qa_registration::Filter>() == Status{});

        const gr::BlockWrapper<qa_registration::Filter> wrapper;
        expect(eq(wrapper.version(), gr::block::kDefaultVersion));
        expect(wrapper.status() == Status{});
        expect(!wrapper.metaInformation().contains(std::pmr::string(gr::block::kVersionMetaKey))) << "nothing to report, nothing reported";
        expect(!wrapper.metaInformation().contains(std::pmr::string(gr::block::kStatusMetaKey)));
    };

    "the declared qualities are independent and may be set together"_test = [] {
        constexpr Status both = gr::block::statusOf<qa_registration::FilterV1>();
        expect(both.deprecated);
        expect(both.experimental);
        expect(both.any());

        const gr::BlockWrapper<qa_registration::FilterV1> wrapper;
        const gr::property_map&                           meta = wrapper.metaInformation();
        expect(eq(meta.at(std::pmr::string(gr::block::kVersionMetaKey)).value_or(gr::Size_t{}), gr::Size_t{1}));
        const auto* flags = meta.at(std::pmr::string(gr::block::kStatusMetaKey)).get_if<gr::property_map>();
        expect(flags != nullptr);
        if (flags != nullptr) {
            expect(flags->at("deprecated").value_or(false));
            expect(flags->at("experimental").value_or(false));
        }
    };

    "two versions of one key coexist, and the newest is what a caller that names none gets"_test = [] {
        gr::BlockRegistry registry;
        expect(qa_registration::insertAs<qa_registration::FilterV1>(registry, "qa::Filter"));
        expect(qa_registration::insertAs<qa_registration::FilterV2>(registry, "qa::Filter")) << "a new version under a known key contributes";

        expect(eq(registry.keys().size(), 1UZ)) << "one key, two versions";
        const std::vector<Version> registered = registry.versions("qa::Filter");
        expect(eq(registered.size(), 2UZ));
        if (registered.size() == 2UZ) {
            expect(eq(registered[0], Version{1U})) << "oldest first";
            expect(eq(registered[1], Version{2U}));
        }
        expect(registry.newestVersion("qa::Filter") == std::optional<Version>{2U});

        const std::unique_ptr<gr::BlockModel> newest = registry.create("qa::Filter", {});
        expect(newest != nullptr);
        if (newest != nullptr) {
            expect(eq(newest->version(), Version{2U}));
            expect(!newest->pinnedVersion().has_value()) << "taking the newest is not pinning";
        }
    };

    "a pinned version is the one that is built, and the instance remembers it was pinned"_test = [] {
        gr::BlockRegistry registry;
        expect(qa_registration::insertAs<qa_registration::FilterV1>(registry, "qa::Filter"));
        expect(qa_registration::insertAs<qa_registration::FilterV2>(registry, "qa::Filter"));

        const std::unique_ptr<gr::BlockModel> pinned = registry.create("qa::Filter", Version{1U}, {});
        expect(pinned != nullptr);
        if (pinned != nullptr) {
            expect(eq(pinned->version(), Version{1U}));
            expect(pinned->pinnedVersion() == std::optional<Version>{1U});
            expect(pinned->status().deprecated);
        }

        expect(registry.create("qa::Filter", Version{7U}, {}) == nullptr) << "a version that was never registered is a miss, not a substitution";
    };

    "the registry reports each version's status without acting on it"_test = [] {
        gr::BlockRegistry registry;
        expect(qa_registration::insertAs<qa_registration::FilterV1>(registry, "qa::Filter"));
        expect(qa_registration::insertAs<qa_registration::FilterV2>(registry, "qa::Filter"));

        expect(registry.status("qa::Filter", 1U) == std::optional<Status>{Status{.deprecated = true, .experimental = true}});
        expect(registry.status("qa::Filter", 2U) == std::optional<Status>{Status{}});
        expect(!registry.status("qa::Filter", 3U).has_value());
        expect(!registry.status("qa::NoSuchBlock", 1U).has_value());
        expect(registry.versions("qa::NoSuchBlock").empty());
        expect(!registry.newestVersion("qa::NoSuchBlock").has_value());
    };

    "one key and one version registered twice is the collision it always was: the last wins"_test = [] {
        gr::BlockRegistry registry;
        expect(qa_registration::insertAs<qa_registration::FilterV1>(registry, "qa::Filter"));
        expect(!qa_registration::insertAs<qa_registration::FilterV1>(registry, "qa::Filter")) << "nothing new";
        expect(eq(registry.versions("qa::Filter").size(), 1UZ));
    };

    "what the generated definition unit hands over carries the declarations"_test = [] {
        const gr::BlockRegistration registration = gr::makeBlockRegistration<qa_registration::FilterV1>(&qa_registration::makeBlock<qa_registration::FilterV1>);
        expect(eq(registration.version, Version{1U}));
        expect(registration.status.deprecated);
        expect(registration.status.experimental);

        gr::BlockRegistry registry;
        expect(gr::insertBlockFactory(registry, registration));
        expect(registry.status(gr::meta::type_name<qa_registration::FilterV1>(), 1U) == std::optional<Status>{registration.status});
    };
};

int main() { /* tests are statically registered */ }
