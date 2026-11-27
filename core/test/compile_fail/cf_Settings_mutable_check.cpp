#include <gnuradio-4.0/Settings.hpp>

// A block whose check hook is not const.
struct MutableCheck {
    struct Settings {
        float gain = 1.f;
        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;
    GR_MAKE_REFLECTABLE(MutableCheck, settings);
    std::optional<gr::Refused> checkSettings(const Settings& /*s*/) { return {}; }
};

int main() {
    MutableCheck                                block;
    gr::settings::BlockSettingsOf<MutableCheck> settings(block);
    return settings.get().has_value() ? 0 : 1;
}
