#include <gnuradio-4.0/annotated.hpp>

// Annotated takes no name: the member's own name is the setting's name, and a string second is no type.
struct NamedTwice {
    gr::Annotated<float, "gain", gr::Unit<"dB">> gain = 0.f;
};

int main() { return NamedTwice{}.gain.value == 0.f ? 0 : 1; }
