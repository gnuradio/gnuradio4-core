#include "abi_probe_plugin.hpp"

/// the probe built from this core's headers like the other probes. It records `ABI_PROBE_PLUGIN_ABI_VERSION`, the plugin
/// ABI version below the one this core implements.
namespace gr::testing {

const bool registered [[maybe_unused]] = static_cast<gr::BlockRegistry&>(abiProbePlugin<(ABI_PROBE_PLUGIN_ABI_VERSION)>()).insert<AbiProbe>("=test::abi_probe_previous");

} // namespace gr::testing

extern "C" {
void GNURADIO_EXPORT gr_plugin_make(gr_plugin_base** plugin) { *plugin = &gr::testing::abiProbePlugin<(ABI_PROBE_PLUGIN_ABI_VERSION)>(); }

void GNURADIO_EXPORT gr_plugin_free(gr_plugin_base*) {}
}
