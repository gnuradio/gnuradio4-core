#include <gnuradio-4.0/Block.hpp>

// A block whose apply hook takes no argument: the hook takes the block's settings.
struct Untaken : gr::Block<Untaken> {
    struct Settings {
        float gain = 1.f;
        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;
    GR_MAKE_REFLECTABLE(Untaken, settings);

    float applied = 0.f;

    void applySettings() { applied = settings.gain; }
};

int main() {
    Untaken block;
    return block.set("gain", 2.f).has_value() ? 0 : 1;
}
