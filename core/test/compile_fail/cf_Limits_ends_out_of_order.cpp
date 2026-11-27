#include <gnuradio-4.0/annotated.hpp>

#include <cstdint>

// equal ends of which one excludes its value: no value passes both.
gr::Annotated<float, gr::Limits<gr::Over<1.f>, gr::Max<1.f>>> member = 1.f;

int main() { return member.value == 0 ? 1 : 0; }
