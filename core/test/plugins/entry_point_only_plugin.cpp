#include <gnuradio-4.0/Plugin.hpp>

/// A plugin that exports `gr_plugin_make` without `gr_plugin_free`. The loader never calls the first.
extern "C" {
void GNURADIO_EXPORT gr_plugin_make(gr_plugin_base** plugin) { *plugin = nullptr; }
}
