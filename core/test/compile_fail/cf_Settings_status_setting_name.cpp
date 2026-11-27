#include <gnuradio-4.0/Block.hpp>

// A block whose struct Status holds a member named like a member of its struct Settings.
struct SharedName : gr::Block<SharedName> {
    struct Settings {
        gr::Annotated<float> gain_db = 0.f;
        GR_MAKE_REFLECTABLE(Settings, gain_db);
    } settings;
    struct Status {
        double gain_db = 0.;
        GR_MAKE_REFLECTABLE(Status, gain_db);
    } status;
    GR_MAKE_REFLECTABLE(SharedName, settings, status);
};

int main() { return gr::settings::detail::declarationTable<SharedName>().keys.empty() ? 1 : 0; }
