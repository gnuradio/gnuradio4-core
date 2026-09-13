#include <gnuradio-4.0/Plugin.hpp>

GR_PLUGIN("Versioned Plugin", "Unknown", "MIT", "v1")

/// a plugin carrying two revisions of one alias, which only the versioned plugin interface can tell apart
namespace good {

struct VersionedFirst : gr::Block<VersionedFirst> {
    using Description = gr::Doc<"the older revision of the block this plugin registers twice">;

    static constexpr gr::block::Version version = 1U;

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(VersionedFirst, in, out);

    explicit VersionedFirst(gr::property_map init = {}) : gr::Block<VersionedFirst>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

struct VersionedSecond : gr::Block<VersionedSecond> {
    using Description = gr::Doc<"the newer revision, which an unpinned create takes">;

    static constexpr gr::block::Version version = 2U;

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(VersionedSecond, in, out);

    explicit VersionedSecond(gr::property_map init = {}) : gr::Block<VersionedSecond>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return 2.0f * value; }
};

} // namespace good

const bool registeredFirst [[maybe_unused]]  = grPluginBlockRegistry().insert<good::VersionedFirst>("=test::versioned");
const bool registeredSecond [[maybe_unused]] = grPluginBlockRegistry().insert<good::VersionedSecond>("=test::versioned");
