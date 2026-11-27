#include <boost/ut.hpp>

#include <optional>
#include <string>

#include <gnuradio-4.0/Settings.hpp>

namespace qa_settings_base {

struct Fields {
    struct Settings {
        float gain = 1.f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;

    GR_MAKE_REFLECTABLE(Fields, settings);
};

// An out-of-tree implementation of SettingsBase must satisfy the entire virtual surface, and must do so again after
// every signature change. This implementation is compiled to verify that the interface still closes, at the point of
// the change rather than in a downstream repository, and it counts each hook the calls dispatch to.
struct MinimalSettings : gr::SettingsBase {
    Fields      fields;
    mutable int nChecks    = 0;
    int         nApplies   = 0;
    int         nResets    = 0;
    int         nDescribes = 0;

    MinimalSettings() : gr::SettingsBase(gr::settings::detail::declarationTable<Fields>(), &fields, nullptr, {}) {}

private:
    [[nodiscard]] bool hasCheckHook() const noexcept override { return true; }

    [[nodiscard]] std::optional<gr::Refused> runCheckHook(const gr::settings::detail::Proposal&) const override {
        ++nChecks;
        return std::nullopt;
    }

    [[nodiscard]] bool hasApplyHook() const noexcept override { return true; }

    [[nodiscard]] bool applyTakesSettings() const noexcept override { return true; }

    void runSettingsApplyHook(const gr::property_map&, const gr::settings::ValueMap&, const gr::settings::ValueMap*) override { ++nApplies; }

    void runResetHook() override { ++nResets; }

    void describeBlock() override { ++nDescribes; }

    [[nodiscard]] std::string uniqueName() const override { return "minimal"; }
};

} // namespace qa_settings_base

const boost::ut::suite<"an out-of-tree SettingsBase implementation"> settingsBaseTests = [] {
    using namespace boost::ut;

    "the virtual surface closes and dispatches"_test = [] {
        qa_settings_base::MinimalSettings settings;
        gr::SettingsBase&                 base = settings;

        gr::settings::detail::FrameworkAccess::describe(base);
        expect(eq(settings.nDescribes, 1)) << "describe() reaches the block's description";

        expect(base.set({{"gain", 2.f}}).has_value());
        expect(eq(settings.fields.settings.gain, 2.f)) << "a block no graph holds applies at the call";
        expect(eq(settings.nChecks, 2)) << "the initialization checks the whole state, then the call runs the check hook";
        expect(eq(settings.nApplies, 2)) << "the first use initializes, then the call applies";
        expect(base.get<float>("gain") == 2.f);

        expect(base.resetDefaults().has_value());
        expect(eq(settings.nResets, 1)) << "a reset runs the reset hook once";
        expect(eq(settings.fields.settings.gain, 1.f));
    };
};
