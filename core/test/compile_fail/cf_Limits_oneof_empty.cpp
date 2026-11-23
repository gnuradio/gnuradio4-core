#include <gnuradio-4.0/annotated.hpp>

#include <cstdint>

// OneOf with no value.
gr::Annotated<int, gr::Limits<gr::OneOf<>>> member = 1;

int main() { return member.value == 0 ? 1 : 0; }
