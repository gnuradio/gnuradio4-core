#include <gnuradio-4.0/annotated.hpp>

#include <cstdint>

// a bound of 300 on an 8-bit setting, which holds at most 255.
gr::Annotated<std::uint8_t, gr::Limits<gr::Max<300>>> member = std::uint8_t{1};

int main() { return member.value == 0 ? 1 : 0; }
