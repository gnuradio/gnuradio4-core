#include <gnuradio-4.0/Block.hpp>

// A block whose struct Settings leaves `offset` off its GR_MAKE_REFLECTABLE, so no host would see it.
struct Unlisted : gr::Block<Unlisted> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<float> gain   = 1.f;
        float                offset = 0.f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;

    GR_MAKE_REFLECTABLE(Unlisted, in, out, settings);

    [[nodiscard]] constexpr float processOne(float x) const noexcept { return x * settings.gain + settings.offset; }
};

int main() { return gr::settings::detail::declarationTable<Unlisted>().keys.empty() ? 1 : 0; }
