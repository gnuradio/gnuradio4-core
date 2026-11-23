#include <gnuradio-4.0/annotated.hpp>

#include <cstdint>

// Max and Under together: two upper ends.
gr::Annotated<int, gr::Limits<gr::Max<9>, gr::Under<10>>> member = 1;

int main() { return member.value == 0 ? 1 : 0; }
