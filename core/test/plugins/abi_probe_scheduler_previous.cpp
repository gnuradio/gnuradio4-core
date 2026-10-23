// SPDX-License-Identifier: MIT
#include <gnuradio-4.0/Plugin.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

namespace {
struct PreviousSchedulerPlugin final : gr_plugin_base {
    std::uint8_t                        abiVersion() const override { return GR_PLUGIN_CURRENT_ABI_VERSION - 1; }
    std::vector<std::string>            availableBlocks() const override { return {}; }
    std::unique_ptr<gr::BlockModel>     createBlock(std::string_view, const gr::property_map&) override { return {}; }
    std::vector<std::string>            availableSchedulers() const override { return {"test::previous_plugin_scheduler"}; }
    std::unique_ptr<gr::SchedulerModel> createScheduler(std::string_view, const gr::property_map&) override { return std::make_unique<gr::SchedulerWrapper<gr::scheduler::Simple<>>>(); }
};
} // namespace

extern "C" {
void GNURADIO_EXPORT gr_plugin_make(gr_plugin_base** plugin) { *plugin = new PreviousSchedulerPlugin; }
void GNURADIO_EXPORT gr_plugin_free(gr_plugin_base* plugin) { delete plugin; }
}
