#include <gnuradio-4.0/annotated.hpp>

#include <cstdint>

// OneOf lists 1, which Min<2> excludes.
gr::Annotated<int, gr::Limits<gr::Min<2>, gr::OneOf<1, 2, 4>>> member = 2;

int main() { return member.value == 0 ? 1 : 0; }
