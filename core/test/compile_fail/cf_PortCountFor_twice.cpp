#include <gnuradio-4.0/Port.hpp>
#include <gnuradio-4.0/SettingDeclaration.hpp>

// Two members size the one collection `inputs`.
struct CountedTwice {
    std::vector<gr::PortIn<float>> inputs{1UZ};
    struct Settings {
        gr::Annotated<gr::Size_t, gr::PortCountFor<"inputs">> n_inputs = 1U;
        gr::Annotated<gr::Size_t, gr::PortCountFor<"inputs">> n_more   = 1U;
        GR_MAKE_REFLECTABLE(Settings, n_inputs, n_more);
    } settings;
    GR_MAKE_REFLECTABLE(CountedTwice, inputs, settings);
};

int main() { return gr::settings::detail::declarationTable<CountedTwice>().keys.empty() ? 1 : 0; }
