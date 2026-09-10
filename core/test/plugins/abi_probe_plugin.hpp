#ifndef GR_TEST_ABI_PROBE_PLUGIN_HPP
#define GR_TEST_ABI_PROBE_PLUGIN_HPP

#include <cstdint>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Plugin.hpp>

/// what the two probe plugins share: one block, and one plugin instance per plugin ABI version they declare
namespace gr::testing {

struct AbiProbe : gr::Block<AbiProbe> {
    using Description = gr::Doc<"passes its input through; carried by a plugin that declares a chosen plugin ABI version">;

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(AbiProbe, in, out);

    explicit AbiProbe(gr::property_map init = {}) : gr::Block<AbiProbe>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

template<std::uint8_t abiVersion>
gr::plugin<abiVersion>& abiProbePlugin() {
    static gr::plugin<abiVersion> instance = [] {
        gr::plugin<abiVersion> result;
        result.metadata = gr_plugin_metadata{.plugin_name = "ABI Probe Plugin", .plugin_author = "Unknown", .plugin_license = "MIT", .plugin_version = "v1"};
        return result;
    }();
    return instance;
}

} // namespace gr::testing

#endif // GR_TEST_ABI_PROBE_PLUGIN_HPP
