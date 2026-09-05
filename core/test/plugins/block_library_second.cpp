#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>

/// a second block library, so that a directory holds more than one and an unload can strand a neighbor's entry
namespace gr::testing {

struct LibraryTripler : gr::Block<LibraryTripler> {
    using Description = gr::Doc<"triples its input, from a second shared object with no plugin interface">;

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(LibraryTripler, in, out);

    explicit LibraryTripler(gr::property_map init = {}) : gr::Block<LibraryTripler>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return 3.0f * value; }
};

const bool registered [[maybe_unused]] = gr::globalBlockRegistry().insert<LibraryTripler>("=test::library_tripler");

} // namespace gr::testing
