#include <gnuradio-4.0/annotated.hpp>

#include <cstdint>

// OneOf naming 2 twice, once as an integer and once as a float.
gr::Annotated<float, gr::Limits<gr::OneOf<1.f, 2, 2.f>>> member = 1.f;

int main() { return member.value == 0 ? 1 : 0; }
