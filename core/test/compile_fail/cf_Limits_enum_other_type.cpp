#include <gnuradio-4.0/annotated.hpp>

namespace cf_limits {
enum class Taper { rectangular, hann, hamming };
enum class Mode { automatic, manual };
} // namespace cf_limits

// OneOf naming an enumerator of another enumeration than the setting's.
gr::Annotated<cf_limits::Taper, gr::Limits<gr::OneOf<cf_limits::Mode::manual>>> member = cf_limits::Taper::hann;

int main() { return member.value == cf_limits::Taper::rectangular ? 1 : 0; }
