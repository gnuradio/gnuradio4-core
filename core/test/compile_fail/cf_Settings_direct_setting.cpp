#include <gnuradio-4.0/Block.hpp>

// A block that declares a setting as a direct member of its own.
struct Direct : gr::Block<Direct> {
    float gain = 1.f;
    GR_MAKE_REFLECTABLE(Direct, gain);
};

int main() { return gr::settings::detail::declarationTable<Direct>().keys.empty() ? 1 : 0; }
