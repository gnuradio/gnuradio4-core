#include <gnuradio-4.0/Block.hpp>

// A block without Stride<> that declares its own `stride`: the base block's `stride` is const here, and its name is
// reserved.
struct OwnStride : gr::Block<OwnStride> {
    struct Settings {
        gr::Size_t stride = 1U;
        GR_MAKE_REFLECTABLE(Settings, stride);
    } settings;
    GR_MAKE_REFLECTABLE(OwnStride, settings);
};

int main() { return gr::settings::detail::declarationTable<OwnStride>().keys.empty() ? 1 : 0; }
