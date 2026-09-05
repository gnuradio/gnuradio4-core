#include <boost/ut.hpp>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>

#include <memory>
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

template<typename TBlock>
std::unique_ptr<gr::BlockModel> makeBlock(gr::property_map params) {
    return std::make_unique<gr::BlockWrapper<TBlock>>(std::move(params));
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

int main() { /* tests are statically registered */ }
