#include <gnuradio-4.0/annotated.hpp>

#include <cstdint>

// Min and Over together: two lower ends.
gr::Annotated<int, gr::Limits<gr::Min<1>, gr::Over<0>>> member = 1;

int main() { return member.value == 0 ? 1 : 0; }
