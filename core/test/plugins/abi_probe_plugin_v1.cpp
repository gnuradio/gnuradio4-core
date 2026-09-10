#include "abi_probe_plugin.hpp"

/// the probe at plugin ABI version 1, standing for a plugin left installed from a core that implemented that version
namespace gr::testing {

const bool registered [[maybe_unused]] = static_cast<gr::BlockRegistry&>(abiProbePlugin<1>()).insert<AbiProbe>("=test::abi_probe_v1");

} // namespace gr::testing

extern "C" {
void GNURADIO_EXPORT gr_plugin_make(gr_plugin_base** plugin) { *plugin = &gr::testing::abiProbePlugin<1>(); }

void GNURADIO_EXPORT gr_plugin_free(gr_plugin_base*) {}
}
