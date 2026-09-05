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

    gr::Annotated<float, "extra gain", gr::Visible, gr::Doc<"applied on top of the doubling">, gr::Unit<"dB">> extra_gain = 1.0f;

    GR_MAKE_REFLECTABLE(LibraryDoubler, in, out, extra_gain);

    explicit LibraryDoubler(gr::property_map init = {}) : gr::Block<LibraryDoubler>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return 2.0f * extra_gain * value; }
};

const bool registered [[maybe_unused]] = gr::globalBlockRegistry().insert<LibraryDoubler>("=test::library_doubler");

} // namespace gr::testing
