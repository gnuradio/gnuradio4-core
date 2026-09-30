#include <memory>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Plugin.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>

/**
 * @brief A plugin at the plugin ABI version this core implements whose load registers a scheduler at version 1.
 *
 * A library a plugin depends on registers into the process-wide scheduler registry from its static initializers, and
 * one built against a core of another version records that version. A loader must refuse the plugin before anything
 * calls the scheduler's factory.
 */
namespace gr::testing {

std::unique_ptr<gr::SchedulerModel> makeForeignScheduler(gr::property_map /*parameters*/) { return nullptr; }

const bool registered [[maybe_unused]] = gr::globalSchedulerRegistry().insert("test::plugin_foreign_scheduler", "", makeForeignScheduler, 1);

gr::plugin<GR_PLUGIN_CURRENT_ABI_VERSION>& foreignSchedulerPlugin() {
    static gr::plugin<GR_PLUGIN_CURRENT_ABI_VERSION> instance;
    return instance;
}

} // namespace gr::testing

extern "C" {
void GNURADIO_EXPORT gr_plugin_make(gr_plugin_base** plugin) { *plugin = &gr::testing::foreignSchedulerPlugin(); }

void GNURADIO_EXPORT gr_plugin_free(gr_plugin_base*) {}
}
