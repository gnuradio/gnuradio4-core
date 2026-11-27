#include <gnuradio-4.0/Block.hpp>

// A block whose check takes its `Settings` but which lists no member of that type.
struct Unlisted : gr::Block<Unlisted> {
    struct Settings {
        float gain = 1.f;
        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;
    GR_MAKE_REFLECTABLE(Unlisted);

    std::optional<gr::Refused> checkSettings(const Settings& s) const { return s.gain > 2.f ? std::optional(gr::Refused("must be <= 2")) : std::nullopt; }
};

int main() {
    Unlisted block;
    return block.check({}).has_value() ? 0 : 1;
}
