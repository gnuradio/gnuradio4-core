#include <boost/ut.hpp>

#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <gnuradio-4.0/Settings.hpp>

namespace qa_settings_base {

// An out-of-tree implementation of SettingsBase must satisfy the entire virtual surface, and must
// do so again after every signature change. This implementation exists to be compiled: it verifies
// that the interface still closes, at the point of the change rather than in a downstream
// repository. It deliberately stores and computes nothing.
struct MinimalSettings : gr::SettingsBase {
    gr::SettingsCtx                                                                  _ctx{};
    std::set<std::string>                                                            _keys{};
    std::map<gr::pmt::Value, std::vector<CtxSettingsPair>, gr::settings::PMTCompare> _stored{};

    [[nodiscard]] bool changed() const noexcept override { return false; }
    void               setChanged(bool) noexcept override {}

    void setInitBlockParameters(const gr::property_map&) override {}
    void init() override {}

    [[nodiscard]] gr::property_map set(const gr::property_map&, gr::SettingsCtx) override { return {}; }
    [[nodiscard]] gr::property_map setStaged(const gr::property_map&) override { return {}; }

    void storeDefaults() override {}
    void resetDefaults() override {}

    [[nodiscard]] const gr::SettingsCtx&         activeContext() const noexcept override { return _ctx; }
    [[nodiscard]] bool                           removeContext(gr::SettingsCtx) override { return false; }
    [[nodiscard]] std::optional<gr::SettingsCtx> activateContext(gr::SettingsCtx) override { return std::nullopt; }
    void                                         autoUpdate(const gr::Tag&) override {}

    [[nodiscard]] gr::property_map                get(std::span<const std::string>) const noexcept override { return {}; }
    [[nodiscard]] std::optional<gr::pmt::Value>   get(const std::string&) const noexcept override { return std::nullopt; }
    [[nodiscard]] std::optional<gr::property_map> getStored(std::span<const std::string>, gr::SettingsCtx) const noexcept override { return std::nullopt; }
    [[nodiscard]] std::optional<gr::pmt::Value>   getStored(const std::string&, gr::SettingsCtx) const noexcept override { return std::nullopt; }
    [[nodiscard]] gr::Size_t                      getNStoredParameters() const noexcept override { return 0U; }
    [[nodiscard]] gr::Size_t                      getNAutoUpdateParameters() const noexcept override { return 0U; }

    [[nodiscard]] std::map<gr::pmt::Value, std::vector<CtxSettingsPair>, gr::settings::PMTCompare> getStoredAll() const noexcept override { return _stored; }

    // the three accessors that changed from reference to value; returning a reference to
    // lock-protected state cannot be made safe, which is why the signatures changed
    [[nodiscard]] gr::property_map stagedParameters() const override { return {}; }
    [[nodiscard]] gr::property_map defaultParameters() const noexcept override { return {}; }
    [[nodiscard]] gr::property_map activeParameters() const noexcept override { return {}; }

    [[nodiscard]] const std::set<std::string>& writableMembers() const override { return _keys; }
    [[nodiscard]] std::set<std::string>        autoUpdateParameters(gr::SettingsCtx) noexcept override { return {}; }
    [[nodiscard]] const std::set<std::string>& autoForwardParameters() const noexcept override { return _keys; }
    void                                       addAutoForwardParameters(std::set<std::string> parameterKeys) override { _keys.merge(parameterKeys); }

    [[nodiscard]] gr::ApplyStagedParametersResult applyStagedParameters() override { return {}; }
    void                                          updateActiveParameters() noexcept override {}
    void                                          loadParametersFromPropertyMap(const gr::property_map&, gr::SettingsCtx) override {}
};

} // namespace qa_settings_base

const boost::ut::suite<"an out-of-tree SettingsBase implementation"> settingsBaseTests = [] {
    using namespace boost::ut;

    "the virtual surface closes and dispatches"_test = [] {
        qa_settings_base::MinimalSettings settings;
        gr::SettingsBase&                 base = settings;

        expect(not base.changed());
        expect(base.get().empty());
        expect(base.stagedParameters().empty());
        expect(base.defaultParameters().empty());
        expect(base.activeParameters().empty());

        base.addAutoForwardParameters({"gain"});
        expect(base.autoForwardParameters().contains("gain")) << "the mutator did not reach the implementation";
    };
};

int main() { /* tests are statically registered */ }
