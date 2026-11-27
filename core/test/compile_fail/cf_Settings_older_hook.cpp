#include <gnuradio-4.0/Settings.hpp>

// A block that defines a member named settingsChanged, which fails to compile.
struct OlderHook {
    struct Settings {
        float gain = 1.f;
        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;
    GR_MAKE_REFLECTABLE(OlderHook, settings);
    void settingsChanged(const gr::property_map& /*oldSettings*/, const gr::property_map& /*newSettings*/) {}
};

int main() {
    OlderHook                                block;
    gr::settings::BlockSettingsOf<OlderHook> settings(block);
    return settings.get().has_value() ? 0 : 1;
}
