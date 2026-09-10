#include "abi_probe_plugin.hpp"

/// the probe at the plugin ABI version this core implements, which a host built from this core loads
namespace gr::testing {

const bool registered [[maybe_unused]] = static_cast<gr::BlockRegistry&>(abiProbePlugin<GR_PLUGIN_CURRENT_ABI_VERSION>()).insert<AbiProbe>("=test::abi_probe");

} // namespace gr::testing

extern "C" {
void GNURADIO_EXPORT gr_plugin_make(gr_plugin_base** plugin) { *plugin = &gr::testing::abiProbePlugin<GR_PLUGIN_CURRENT_ABI_VERSION>(); }

void GNURADIO_EXPORT gr_plugin_free(gr_plugin_base*) {}
}
