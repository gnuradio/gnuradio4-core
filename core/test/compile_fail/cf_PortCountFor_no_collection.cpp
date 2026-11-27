#include <gnuradio-4.0/Port.hpp>
#include <gnuradio-4.0/SettingDeclaration.hpp>

// PortCountFor naming `taps`, which is no dynamic port collection of the block.
struct CountOfTaps {
    std::vector<gr::PortIn<float>> inputs{1UZ};
    struct Settings {
        std::vector<float>                                  taps{};
        gr::Annotated<gr::Size_t, gr::PortCountFor<"taps">> n_taps = 1U;
        GR_MAKE_REFLECTABLE(Settings, taps, n_taps);
    } settings;
    GR_MAKE_REFLECTABLE(CountOfTaps, inputs, settings);
};

int main() { return gr::settings::detail::declarationTable<CountOfTaps>().keys.empty() ? 1 : 0; }
