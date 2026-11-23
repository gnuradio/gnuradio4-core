#include <gnuradio-4.0/Port.hpp>
#include <gnuradio-4.0/SettingDeclaration.hpp>

// PortCountFor on a float member: a port count is an integer.
struct CountOnFloat {
    std::vector<gr::PortIn<float>> inputs{1UZ};
    struct Settings {
        gr::Annotated<float, gr::PortCountFor<"inputs">> n_inputs = 1.f;
        GR_MAKE_REFLECTABLE(Settings, n_inputs);
    } settings;
    GR_MAKE_REFLECTABLE(CountOnFloat, inputs, settings);
};

int main() { return gr::settings::detail::declarationTable<CountOnFloat>().keys.empty() ? 1 : 0; }
