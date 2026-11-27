#include <algorithm>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>

/**
 * @brief A shared object that carries a block but is not a plugin.
 *
 * It exports no `gr_plugin_make`; its entry reaches the registry from the static initializer below, as soon as the
 * object is mapped. A loader must not unload it afterwards: the registry entry points into this object's code.
 */
namespace gr::testing {

struct LibraryDoubler : gr::Block<LibraryDoubler> {
    using Description = gr::Doc<"doubles its input, from a shared object that carries no plugin interface">;

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<float, gr::Visible, gr::Doc<"applied on top of the doubling">, gr::Unit<"dB">, gr::Limits<gr::Over<0.f>, gr::Max<100.f>>> extra_gain = 1.0f;

        GR_MAKE_REFLECTABLE(Settings, extra_gain);
    } settings;

    struct Status {
        gr::Annotated<float, gr::Doc<"largest output seen">> peak = 0.0f;

        GR_MAKE_REFLECTABLE(Status, peak);
    } status;

    GR_MAKE_REFLECTABLE(LibraryDoubler, in, out, settings, status);

    explicit LibraryDoubler(gr::property_map init = {}) : gr::Block<LibraryDoubler>(std::move(init)) {}

    [[nodiscard]] float processOne(float value) noexcept {
        const float doubled = 2.0f * settings.extra_gain * value;
        status.peak         = std::max(status.peak.value, doubled);
        return doubled;
    }
};

const bool registered [[maybe_unused]] = gr::globalBlockRegistry().insert<LibraryDoubler>("=test::library_doubler");

} // namespace gr::testing
