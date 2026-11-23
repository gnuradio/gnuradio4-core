#include <gnuradio-4.0/Settings.hpp>

// A block that defines two overloads of a member named settingsChanged, which fails to compile.
struct OlderHookOverloads {
    struct Settings {
        float gain = 1.f;
        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;
    GR_MAKE_REFLECTABLE(OlderHookOverloads, settings);
    void settingsChanged(const gr::property_map& /*oldSettings*/, const gr::property_map& /*newSettings*/) {}
    void settingsChanged(const gr::property_map& /*oldSettings*/, const gr::property_map& /*newSettings*/, gr::property_map& /*forwardSettings*/) {}
};

int main() {
    OlderHookOverloads                                block;
    gr::settings::BlockSettingsOf<OlderHookOverloads> settings(block);
    return settings.get().has_value() ? 0 : 1;
}
