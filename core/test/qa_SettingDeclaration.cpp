#include <boost/ut.hpp>

#include <array>
#include <atomic>
#include <clocale>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#ifdef _MSC_VER
#include <malloc.h>
#endif
#include <expected>
#include <format>
#include <limits>
#include <memory_resource>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/SettingDeclaration.hpp>

// the replaced operator new allocates with malloc, so the replaced operator delete frees with free
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

namespace {

// counts the allocations of the thread that set `countAllocations`
std::atomic<std::size_t> gAllocations{0UZ};
thread_local bool        countAllocations = false;

} // namespace

void* operator new(std::size_t size) {
    if (countAllocations) {
        gAllocations.fetch_add(1UZ, std::memory_order_relaxed);
    }
    if (void* memory = std::malloc(size == 0UZ ? 1UZ : size); memory != nullptr) {
        return memory;
    }
    throw std::bad_alloc();
}

// std::pmr::new_delete_resource allocates through the aligned form
void* operator new(std::size_t size, std::align_val_t alignment) {
    if (countAllocations) {
        gAllocations.fetch_add(1UZ, std::memory_order_relaxed);
    }
    const std::size_t align = static_cast<std::size_t>(alignment);
#ifdef _MSC_VER // MSVC's C runtime has no aligned_alloc
    void* memory = _aligned_malloc(size == 0UZ ? align : size, align);
#else
    void* memory = std::aligned_alloc(align, (size + align - 1UZ) / align * align + (size == 0UZ ? align : 0UZ));
#endif
    if (memory != nullptr) {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
#ifdef _MSC_VER
void operator delete(void* memory, std::align_val_t) noexcept { _aligned_free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { _aligned_free(memory); }
#else
void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
#endif

namespace qa_setting_declaration {

using gr::property_map;
using gr::pmt::Value;
using gr::settings::detail::Declaration;
using gr::settings::detail::DeclarationTable;

enum class Mode { automatic, manual };
enum class Taper { rectangular, hann, hamming };

// one setting per kind of predicate in Limits, for the metadata a tool reads
struct Bounded : gr::Block<Bounded> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<float, gr::Limits<gr::Over<0.f>, gr::Max<0.5f>>>           cutoff     = 0.25f;
        gr::Annotated<gr::Size_t, gr::Limits<gr::OneOf<1U, 2U, 4U>>>             decimation = 2U;
        gr::Annotated<gr::Size_t, gr::Limits<gr::Min<1U>>>                       taps       = 8U;
        gr::Annotated<Taper, gr::Limits<gr::OneOf<Taper::hann, Taper::hamming>>> taper      = Taper::hann;

        GR_MAKE_REFLECTABLE(Settings, cutoff, decimation, taps, taper);
    } settings;

    GR_MAKE_REFLECTABLE(Bounded, in, out, settings);

    [[nodiscard]] constexpr float processOne(float x) const noexcept { return x; }
};

struct Mixer : gr::Block<Mixer> {
    using Description = gr::Doc<"a reflected block that declares its keys by annotations">;

    std::vector<gr::PortIn<float>> inputs{2UZ};
    gr::PortOut<float>             out;

    struct Settings {
        gr::Annotated<float, gr::Unit<"S/s">, gr::Doc<"the input's rate">, gr::Visible, gr::Limits<gr::Min<1.f>, gr::Max<1e7f>>> sample_rate = 1e6f;
        double                                                                                                                   ppm         = 0.0;
        gr::Annotated<gr::Size_t, gr::StoppedOnly>                                                                               burst_size  = 16U;
        gr::Annotated<gr::Size_t, gr::PortCountFor<"inputs">, gr::StoppedOnly>                                                   n_inputs    = 2U;
        Mode                                                                                                                     mode        = Mode::automatic;
        std::vector<float>                                                                                                       taps        = {1.f};
        std::array<double, 2UZ>                                                                                                  corner      = {0.0, 1.0};
        std::complex<float>                                                                                                      offset      = {0.f, 0.f};
        bool                                                                                                                     enabled     = true;
        const std::int32_t                                                                                                       revision    = 3;

        GR_MAKE_REFLECTABLE(Settings, sample_rate, ppm, burst_size, n_inputs, mode, taps, corner, offset, enabled, revision);
    } settings;

    struct Status {
        gr::Annotated<float> agc_gain = 0.f;

        GR_MAKE_REFLECTABLE(Status, agc_gain);
    } status;

    GR_MAKE_REFLECTABLE(Mixer, inputs, out, settings, status);

    gr::work::Status processBulk(auto&, auto&) { return gr::work::Status::OK; }
};

// one key for each update policy, port declaration and access the annotations and the struct Status export
struct Exported : gr::Block<Exported> {
    using Description = gr::Doc<"a reflected block with one key per exported annotation">;

    std::vector<gr::PortIn<float>>  inputs{1UZ};
    std::vector<gr::PortOut<float>> outputs{1UZ};
    std::vector<gr::PortOut<float>> extras{1UZ};

    struct Settings {
        gr::Annotated<float, gr::StoppedOnly>                                                                               stopped_key  = 0.f;
        gr::Annotated<gr::Size_t, gr::PortCountFor<"inputs">, gr::Limits<gr::Min<gr::Size_t{1U}>, gr::Max<gr::Size_t{8U}>>> n_inputs     = 1U;
        gr::Annotated<gr::Size_t, gr::PortCountFor<"outputs", "extras">, gr::StoppedOnly>                                   n_outputs    = 1U;
        float                                                                                                               plain_key    = 0.f;
        std::size_t                                                                                                         sample_count = 0UZ;

        GR_MAKE_REFLECTABLE(Settings, stopped_key, n_inputs, n_outputs, plain_key, sample_count);
    } settings;

    struct Status {
        gr::Annotated<double> level = 0.0;

        GR_MAKE_REFLECTABLE(Status, level);
    } status;

    GR_MAKE_REFLECTABLE(Exported, inputs, outputs, extras, settings, status);

    gr::work::Status processBulk(auto&, auto&, auto&) { return gr::work::Status::OK; }
};

struct Radio : gr::Block<Radio> {
    using Description = gr::Doc<"a described radio with a bare channel group">;

    gr::PortIn<float>               in;
    std::vector<gr::PortOut<float>> outputs{2UZ};

    struct Settings {
        gr::Annotated<gr::Size_t, gr::StoppedOnly> burst_size = 16U;

        GR_MAKE_REFLECTABLE(Settings, burst_size);
    } settings;

    GR_MAKE_REFLECTABLE(Radio, in, outputs, settings);

    property_map describedSettings;

    static property_map settingsDescription() {
        return property_map{
            {"version", 1},
            {"block_type", "qa::Radio"},
            {"documentation", "a radio with one receive channel per output"},
            {"settings",
                property_map{
                    {"serial", property_map{{"type", "str"}, {"default", ""}, {"update_policy", "stopped"}}},
                    {"sample_rate", property_map{{"type", "float64"}, {"default", 10.0e6}, {"minimum", 2.0e6}, {"maximum", 20.0e6}, {"step", 1.0e3}, {"unit", "S/s"}}},
                    {"n_channels", property_map{{"type", "uint32"}, {"default", 2}, {"port_count_for", "outputs"}}},
                    {"antenna", property_map{{"type", "str"}, {"default", "RX"}, {"choices", std::vector<std::string>{"RX", "TX/RX"}}}},
                    {"bias_tee", property_map{{"type", "bool"}, {"default", false}, {"visible", true}}},
                    {"bias_voltage", property_map{{"type", "float64"}, {"default", 3.3}, {"enabled_when_parameter", "bias_tee"}, {"enabled_when_value", true}}},
                    {"taps", property_map{{"type", "float32_vector"}}},
                    {"temperature", property_map{{"type", "float64"}, {"unit", "degC"}, {"access", "read_only"}}},
                    {"uptime", property_map{{"type", "uint64"}, {"default", std::uint64_t{0}}, {"access", "read_only"}}},
                }},
            {"groups", property_map{{"channel",
                           property_map{
                               {"count", "n_channels"},
                               {"bare", true},
                               {"settings",
                                   property_map{
                                       {"frequency", property_map{{"type", "float64"}, {"default", 100.0e6}, {"minimum", 1.0e6}, {"maximum", 6.0e9}, {"unit", "Hz"}}},
                                       {"lna", property_map{{"type", "float64"}, {"minimum", 0.0}, {"maximum", 40.0}}},
                                       {"rssi", property_map{{"type", "float32"}, {"unit", "dBFS"}, {"access", "read_only"}}},
                                   }},
                           }}}},
        };
    }

    gr::work::Status processBulk(auto&, auto&) { return gr::work::Status::OK; }
};

// a described group counted by a reflected port key
struct Tuner : gr::Block<Tuner> {
    using Description = gr::Doc<"a tuner whose channel count is a reflected field">;

    gr::PortIn<float>               in;
    std::vector<gr::PortOut<float>> outputs{3UZ};

    struct Settings {
        gr::Annotated<gr::Size_t, gr::PortCountFor<"outputs">> n_channels = 3U;

        GR_MAKE_REFLECTABLE(Settings, n_channels);
    } settings;

    GR_MAKE_REFLECTABLE(Tuner, in, outputs, settings);

    property_map describedSettings;

    static property_map settingsDescription() {
        return property_map{
            {"version", 1},
            {"block_type", "qa::Tuner"},
            {"groups", property_map{{"channel", property_map{{"count", "n_channels"}, {"settings", property_map{{"frequency", property_map{{"type", "float64"}, {"default", 100.0e6}}}}}}}}},
        };
    }

    gr::work::Status processBulk(auto&, auto&) { return gr::work::Status::OK; }
};

// a block with an input collection its port key sizes, one reflected setting, and the description of fault case kCase
template<std::size_t kCase>
struct Faulty {
    std::vector<gr::PortIn<float>>  inputs{1UZ};
    std::vector<gr::PortOut<float>> outputs{1UZ};

    struct Settings {
        gr::Annotated<gr::Size_t, gr::PortCountFor<"inputs">> n_inputs = 1U;
        float                                                 gain     = 1.f;

        GR_MAKE_REFLECTABLE(Settings, n_inputs, gain);
    } settings;

    property_map describedSettings;

    GR_MAKE_REFLECTABLE(Faulty, inputs, outputs, settings);

    static property_map settingsDescription();
};

[[nodiscard]] property_map setting(std::string_view type, property_map fields = {}) {
    fields.insert_or_assign(std::pmr::string("type"), Value(type));
    return fields;
}

[[nodiscard]] property_map description(property_map settings, property_map extra = {}) {
    property_map result{{"version", 1}, {"block_type", "qa::Faulty"}, {"settings", std::move(settings)}};
    for (auto& [key, value] : extra) {
        result.insert_or_assign(key, value);
    }
    return result;
}

struct FaultCase {
    std::string_view rule;
    property_map (*makeDescription)();
    std::string_view fragment; // the reason names the fault by this text
};

// one entry per faulty item of the description format, each refused with one entry
inline const std::array kFaultCases = std::to_array<FaultCase>({
    {"a field the format does not define", [] { return description({{"rate", setting("float64", {{"minimun", 0.0}, {"default", 1.0}})}}); }, "unknown field 'minimun'"},
    {"an undefined field at the top level", [] { return description({}, {{"settngs", property_map{}}}); }, "unknown field 'settngs'"},
    {"an undefined field in a group", [] { return description({}, {{"groups", property_map{{"channel", property_map{{"cuont", "n_inputs"}}}}}}); }, "unknown field 'cuont'"},
    {"a status map, which the format does not define", [] { return description({}, {{"status", property_map{{"level", setting("float32")}}}}); }, "unknown field 'status'"},
    {"a status map in a group", [] { return description({}, {{"groups", property_map{{"channel", property_map{{"status", property_map{{"rssi", setting("float32")}}}}}}}}); }, "unknown field 'status'"},
    {"no version", [] { return property_map{{"block_type", "qa::Faulty"}}; }, "description: no version"},
    {"a version other than 1", [] { return property_map{{"version", 2}, {"block_type", "qa::Faulty"}}; }, "version not 1: 2"},
    {"no block type", [] { return property_map{{"version", 1}}; }, "description: no block_type"},
    {"a block type that is no text", [] { return property_map{{"version", 1}, {"block_type", 7}}; }, "block_type not a text"},
    {"a setting with no type", [] { return description({{"rate", property_map{{"default", 1.0}}}}); }, "'rate': no type"},
    {"a type the format does not define", [] { return description({{"rate", setting("float128")}}); }, "unknown type 'float128'"},
    {"a default that does not convert", [] { return description({{"rate", setting("float64", {{"default", "fast"}})}}); }, "'rate': default not a decimal number"},
    {"a limit that does not convert", [] { return description({{"count", setting("uint8", {{"maximum", 300}})}}); }, "'count': maximum out of range for uint8"},
    {"a step that does not convert", [] { return description({{"count", setting("int32", {{"step", 0.5}})}}); }, "'count': step not a whole number"},
    {"a default outside its limits", [] { return description({{"rate", setting("float64", {{"default", 50.0}, {"minimum", 0.0}, {"maximum", 10.0}})}}); }, "'rate': default out of range [0, 10]: 50"},
    {"a default outside its list of values", [] { return description({{"antenna", setting("str", {{"default", "LNA"}, {"choices", std::vector<std::string>{"RX", "TX"}}})}}); }, "'antenna': default not one of: RX, TX"},
    {"a range on a bool setting", [] { return description({{"enabled", setting("bool", {{"minimum", false}})}}); }, "range on a bool setting"},
    {"a range on a text setting", [] { return description({{"label", setting("str", {{"maximum", "z"}})}}); }, "range on a str setting"},
    {"a list of values on a setting that is no text", [] { return description({{"rate", setting("float64", {{"choices", std::vector<std::string>{"1", "2"}}})}}); }, "choices on a float64 setting"},
    {"an update policy other than stopped", [] { return description({{"rate", setting("float64", {{"update_policy", "sometimes"}})}}); }, "update_policy 'sometimes' not one of: stopped"},
    {"an access other than read_only", [] { return description({{"rate", setting("float64", {{"access", "write_only"}})}}); }, "access 'write_only' not one of: read_only"},
    {"a field the format does not define", [] { return description({{"rate", setting("float64", {{"not_saved", true}})}}); }, "field 'not_saved'"},
    {"a port key declared live", [] { return description({{"n_outputs", setting("uint32", {{"default", 1}, {"port_count_for", "outputs"}, {"update_policy", "live"}})}}); }, "update_policy 'live'"},
    {"a port key declared read-only", [] { return description({{"n_outputs", setting("uint32", {{"port_count_for", "outputs"}, {"access", "read_only"}})}}); }, "read-only port key"},
    {"a port_count_for that is neither a text nor a list of texts", [] { return description({{"n_outputs", setting("uint32", {{"default", 1}, {"port_count_for", 3}})}}); }, "port_count_for not a text or a list of texts"},
    {"a group with a block-wide setting's name", [] { return description({{"channel", setting("float32")}}, {{"groups", property_map{{"channel", property_map{}}}}}); }, "group 'channel': name of a block-wide setting"},
    {"a count that names no setting", [] { return description({}, {{"groups", property_map{{"channel", property_map{{"count", "n_channels"}}}}}}); }, "count 'n_channels' not a setting"},
    {"a count that names a setting that is no port key", [] { return description({}, {{"groups", property_map{{"channel", property_map{{"count", "gain"}}}}}}); }, "count 'gain' not a port key"},
    {"a port key on a setting of a member group", [] { return description({}, {{"groups", property_map{{"channel", property_map{{"settings", property_map{{"taps", setting("uint32", {{"port_count_for", "inputs"}})}}}}}}}}); }, "port_count_for not allowed in a member group"},
    {"a port key that is no integer setting", [] { return description({{"width", setting("float32", {{"port_count_for", "outputs"}})}}); }, "port key 'width': not an integer setting"},
    {"a port key that names no dynamic port collection", [] { return description({{"n_taps", setting("uint32", {{"default", 1}, {"port_count_for", "taps"}})}}); }, "'taps' not a dynamic port collection"},
    {"two port keys sizing one collection", [] { return description({{"n_more", setting("uint32", {{"default", 1}, {"port_count_for", "inputs"}})}}); }, "two counts of 'inputs'"},
    {"a condition on a missing setting", [] { return description({{"bias", setting("float64", {{"enabled_when_parameter", "bias_tee"}, {"enabled_when_value", true}})}}); }, "condition 'bias_tee' not a setting of the block"},
    {"a condition on a setting that is no bool", [] { return description({{"bias", setting("float64", {{"enabled_when_parameter", "gain"}, {"enabled_when_value", true}})}}); }, "condition 'gain' not a bool setting"},
    {"a condition with no value", [] { return description({{"bias_tee", setting("bool")}, {"bias", setting("float64", {{"enabled_when_parameter", "bias_tee"}})}}); }, "enabled_when_value missing or not a bool"},
    {"a condition whose value is no bool", [] { return description({{"bias_tee", setting("bool")}, {"bias", setting("float64", {{"enabled_when_parameter", "bias_tee"}, {"enabled_when_value", 1}})}}); }, "enabled_when_value missing or not a bool"},
    {"conditions that form a cycle", [] { return description({{"a", setting("bool", {{"enabled_when_parameter", "b"}, {"enabled_when_value", true}})}, {"b", setting("bool", {{"enabled_when_parameter", "a"}, {"enabled_when_value", true}})}}); }, "cycle of conditions"},
    {"a minimum above its maximum", [] { return description({{"rate", setting("float64", {{"minimum", 10.0}, {"maximum", 0.0}})}}); }, "minimum above maximum"},
    {"a described notification level", [] { return description({{"notification_level", setting("float32")}}); }, "notification_level: reserved for a scheduler"},
    {"a name both sources declare", [] { return description({{"gain", setting("float32")}}); }, "setting 'gain': declared by a reflected field and by the description"},
    {"two bare groups", [] { return description({}, {{"groups", property_map{{"rx", property_map{{"bare", true}}}, {"tx", property_map{{"bare", true}}}}}}); }, "two bare groups"},
    {"a bare group setting with a block-wide name", [] { return description({{"frequency", setting("float64")}}, {{"groups", property_map{{"channel", property_map{{"bare", true}, {"settings", property_map{{"frequency", setting("float64")}}}}}}}}); }, "setting 'channel.frequency': bare name of a block-wide key"},
    {"a name that is no identifier", [] { return description({{"rx.gain", setting("float64")}}); }, "name not an identifier: 'rx.gain'"},
});

template<std::size_t kCase>
property_map Faulty<kCase>::settingsDescription() {
    return kFaultCases[kCase].makeDescription();
}

// a scheduler-style block group, whose table holds the notification level
struct SchedulerGroup {
    struct Settings {
        float timeout = 0.f;
        GR_MAKE_REFLECTABLE(Settings, timeout);
    } settings;
    GR_MAKE_REFLECTABLE(SchedulerGroup, settings);
    static constexpr gr::block::Category blockCategory = gr::block::Category::ScheduledBlockGroup;
};

// a tuner that holds its settings in its struct `settings` and reports a gain in its struct `status`
template<typename T>
struct StructTuner : gr::Block<StructTuner<T>> {
    using Description = gr::Doc<"a tuner over a set of inputs">;

    std::vector<gr::PortIn<T>> inputs{1UZ};
    gr::PortOut<T>             out;

    struct Settings {
        gr::Annotated<float, gr::Unit<"S/s">, gr::Doc<"the input's rate">, gr::Visible, gr::Limits<gr::Min<1.f>, gr::Max<1e7f>>> sample_rate = 1e6f;
        gr::Annotated<gr::Size_t, gr::PortCountFor<"inputs">, gr::StoppedOnly>                                                   n_inputs    = 1U;
        Mode                                                                                                                     mode        = Mode::automatic;
        std::vector<T>                                                                                                           taps        = {T{1}};
        gr::Annotated<std::pmr::string>                                                                                          label       = std::pmr::string("tuner");
        gr::Annotated<float>                                                                                                     level       = 0.f;

        GR_MAKE_REFLECTABLE(Settings, sample_rate, n_inputs, mode, taps, label, level);
    } settings;

    struct Status {
        gr::Annotated<float> agc_gain = 0.f;

        GR_MAKE_REFLECTABLE(Status, agc_gain);
    } status;

    GR_MAKE_REFLECTABLE(StructTuner, inputs, out, settings, status);

    gr::work::Status processBulk(auto&, auto&) { return gr::work::Status::OK; }
};

[[nodiscard]] const Declaration& declarationOf(const DeclarationTable& table, std::string_view key) {
    const std::optional<gr::settings::detail::KeyRef> found = table.find(key);
    if (!found) {
        throw std::runtime_error(std::format("no declaration of '{}'", key));
    }
    return *found->declaration;
}

[[nodiscard]] std::string metaText(const property_map& meta, std::string_view key) {
    const auto it = meta.find(key);
    return it != meta.end() ? std::string(it->second.value_or(std::string_view{})) : std::string("<none>");
}

[[nodiscard]] std::string faultText(const DeclarationTable& table) {
    std::string text;
    for (const gr::settings::Rejection& fault : table.faults) {
        text += fault.reason + '\n';
    }
    return text;
}

template<typename TBlock>
void expectOneFault(std::string_view rule, std::string_view fragment) {
    using namespace boost::ut;
    const DeclarationTable& table = gr::settings::detail::declarationTable<TBlock>();
    expect(eq(table.faults.size(), 1UZ)) << rule << ":\n" << faultText(table);
    if (!table.faults.empty()) {
        expect(table.faults.front().key.empty()) << rule;
        expect(table.faults.front().reason.contains(fragment)) << rule << ": expected '" << fragment << "' in " << table.faults.front().reason;
    }
}

} // namespace qa_setting_declaration

const boost::ut::suite<"declaration table"> _declarationTable = [] {
    using namespace boost::ut;
    using namespace qa_setting_declaration;
    using gr::settings::detail::declarationTable;

    "a block with no bare group has no bare names"_test = [] { expect(!declarationTable<Mixer>().find("frequency").has_value()); };

    "a lookup allocates nothing"_test = [] {
        const DeclarationTable& table = declarationTable<Radio>();
        std::size_t             found = 0UZ;
        gAllocations.store(0UZ);
        countAllocations = true;
        for (std::string_view key : {"sample_rate", "frequency", "channel.1.frequency", "channel.12.rssi", "channel.01.frequency", "temperature", "notification_level", "input_chunk_size", "undeclared_but_long_key_name"}) {
            found += table.find(key).has_value() ? 1UZ : 0UZ;
        }
        countAllocations = false;
        expect(eq(gAllocations.load(), 0UZ));
        expect(eq(found, 6UZ));
    };
};

const boost::ut::suite<"faulty declarations"> _faultyDeclarations = [] {
    using namespace boost::ut;
    using namespace qa_setting_declaration;

    "each faulty description is refused with one entry that names the fault"_test = [] { [&]<std::size_t... kCase>(std::index_sequence<kCase...>) { (expectOneFault<Faulty<kCase>>(kFaultCases[kCase].rule, kFaultCases[kCase].fragment), ...); }(std::make_index_sequence<std::tuple_size_v<decltype(kFaultCases)>>()); };
};

const boost::ut::suite<"convert"> _convert = [] {
    using namespace boost::ut;
    using namespace qa_setting_declaration;
    using gr::settings::detail::convert;
    using gr::settings::detail::declarationTable;
    using gr::settings::detail::refuseByLimits;

    // the text a host reads for a value Mixer's key refuses, or nothing for one it takes
    static constexpr auto mixerRefusal = [](std::string_view key, const Value& sent) {
        const std::expected<Value, std::string> converted = convert(declarationOf(declarationTable<Mixer>(), key), sent);
        return converted ? std::string{} : converted.error();
    };

    "an integer sent to a floating-point key converts"_test = [] {
        const auto converted = convert(declarationOf(declarationTable<Mixer>(), "ppm"), Value(std::int32_t{7}));
        expect(fatal(converted.has_value()));
        expect(*converted == Value(7.0));
    };

    "a text for a numeric key may carry whitespace around it and a sign"_test = [] {
        const Declaration& ppm   = declarationOf(declarationTable<Mixer>(), "ppm");
        const Declaration& burst = declarationOf(declarationTable<Mixer>(), "burst_size");
        expect(convert(ppm, Value(" 5e6 ")) == std::expected<Value, std::string>(Value(5e6)));
        expect(convert(burst, Value("\t7\n")) == std::expected<Value, std::string>(Value(std::uint32_t{7})));
        expect(convert(burst, Value("+5")) == std::expected<Value, std::string>(Value(std::uint32_t{5})));
        expect(convert(ppm, Value("+1.5")) == std::expected<Value, std::string>(Value(1.5)));
        expect(convert(ppm, Value("-2.5e-1")) == std::expected<Value, std::string>(Value(-0.25)));
        expect(convert(ppm, Value(".5")) == std::expected<Value, std::string>(Value(0.5)));
        for (std::string_view text : {"++5", "+-5", "- 5", "5e", "e5", ".", "1.2.3", "1,5", "5 5", "   ", "nan", "-inf", "infinity", "1e5.0", "true", "5 # five"}) {
            expect(!convert(ppm, Value(text)).has_value()) << text;
        }
    };

    "a nonzero text that reads as 0 or as an infinity is refused"_test = [] {
        const Declaration& ppm        = declarationOf(declarationTable<Mixer>(), "ppm");
        const Declaration& sampleRate = declarationOf(declarationTable<Mixer>(), "sample_rate");
        const Declaration& burst      = declarationOf(declarationTable<Mixer>(), "burst_size");
        expect(!convert(ppm, Value("1e-400")).has_value());
        expect(!convert(ppm, Value("-1e-400")).has_value());
        expect(!convert(ppm, Value("1e400")).has_value());
        expect(!convert(sampleRate, Value("1e-50")).has_value()) << "0 as a float";
        expect(!convert(sampleRate, Value("1e39")).has_value()) << "past the float range";
        expect(!convert(burst, Value("1e-400")).has_value());
        expect(convert(ppm, Value("1e-300")).has_value());
        expect(convert(sampleRate, Value("1e-30")).has_value());
        for (std::string_view zero : {"0", "-0", "0.0", "0e5", "000", "0.0e-400"}) {
            expect(convert(ppm, Value(zero)) == std::expected<Value, std::string>(Value(0.0))) << zero;
        }
    };

    "a text reads alike under a locale with a decimal comma"_test = [] {
        const Declaration& ppm      = declarationOf(declarationTable<Mixer>(), "ppm");
        const std::string  previous = std::setlocale(LC_NUMERIC, nullptr);
        bool               commaSet = false;
        for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "fr_FR"}) {
            if (std::setlocale(LC_NUMERIC, name) != nullptr) {
                commaSet = true;
                break;
            }
        }
        const auto converted = convert(ppm, Value("2.5"));
        const auto comma     = convert(ppm, Value("2,5"));
        std::setlocale(LC_NUMERIC, previous.c_str());
        expect(converted == std::expected<Value, std::string>(Value(2.5))) << (commaSet ? "under a decimal-comma locale" : "no decimal-comma locale is installed");
        expect(!comma.has_value());
    };

    "a text true or false in any letter case, 1 or 0 converts for a bool key"_test = [] {
        const Declaration& enabled = declarationOf(declarationTable<Mixer>(), "enabled");
        for (std::string_view text : {"true", "TRUE", "True", "tRuE", "1"}) {
            expect(convert(enabled, Value(text)) == std::expected<Value, std::string>(Value(true))) << text;
        }
        for (std::string_view text : {"false", "FALSE", "False", "0"}) {
            expect(convert(enabled, Value(text)) == std::expected<Value, std::string>(Value(false))) << text;
        }
        for (std::string_view text : {"yes", " true", "truee", "tru", "2", "1.0", "on", ""}) {
            expect(!convert(enabled, Value(text)).has_value()) << text;
        }
    };

    "a floating-point key refuses a value outside its type"_test = [] {
        const Declaration& sampleRate = declarationOf(declarationTable<Mixer>(), "sample_rate");
        expect(eq(mixerRefusal("sample_rate", Value(1e300)), std::string("out of range for float32: 1e+300")));
        expect(convert(sampleRate, Value(std::numeric_limits<double>::infinity())).has_value());
    };

    "a complex value converts part by part"_test = [] {
        const auto converted = convert(declarationOf(declarationTable<Mixer>(), "offset"), Value(std::complex<double>{0.5, -1.0}));
        expect(fatal(converted.has_value()));
        expect(*converted == Value(std::complex<float>{0.5f, -1.0f}));
    };

    "a real number for a complex key converts with a zero imaginary part"_test = [] {
        const Declaration& offset = declarationOf(declarationTable<Mixer>(), "offset");
        expect(convert(offset, Value(1.5)) == std::expected<Value, std::string>(Value(std::complex<float>{1.5f, 0.f})));
        expect(convert(offset, Value(std::int32_t{-2})) == std::expected<Value, std::string>(Value(std::complex<float>{-2.f, 0.f})));
        expect(eq(mixerRefusal("offset", Value(1e300)), std::string("out of range for complex32: 1e+300"))) << "a real part outside the part's type";
        expect(eq(mixerRefusal("offset", Value(true)), std::string("not of type complex32: true")));
        expect(!convert(offset, Value("1.5")).has_value());
    };

    "the notification level takes a level's name or the empty text"_test = [] {
        const Declaration& level = declarationOf(declarationTable<SchedulerGroup>(), "notification_level");
        const auto         loud  = convert(level, Value("loud"));
        expect(fatal(loud.has_value()));
        expect(refuseByLimits(level, *loud) == std::optional<std::string>("not one of: trace, debug, info, warning, error, critical, off (or empty): loud"));
        expect(!refuseByLimits(level, Value("")).has_value());
        expect(!refuseByLimits(level, Value("info")).has_value());
    };
};

const boost::ut::suite<"describe"> _describe = [] {
    using namespace boost::ut;
    using namespace qa_setting_declaration;

    "describe counts a group by its reflected count field"_test = [] {
        Tuner tuner;
        gr::settings::detail::describe(tuner);
        expect(tuner.describedSettings.contains("channel.2.frequency"));
        expect(!tuner.describedSettings.contains("channel.3.frequency"));
        Tuner single;
        expect(fatal(single.set("n_channels", gr::Size_t{1}).has_value()));
        gr::settings::detail::describe(single);
        expect(single.describedSettings.contains("channel.0.frequency"));
        expect(!single.describedSettings.contains("channel.1.frequency")) << "the field's value counts, not its default";
    };

    "describe writes each key's metadata"_test = [] {
        Radio radio;
        gr::settings::detail::describe(radio);
        const property_map& meta = radio.meta_information.value;
        expect(eq(metaText(meta, "description"), std::string("a radio with one receive channel per output")));
        expect(eq(metaText(meta, "n_channels::update_policy"), std::string("topology")));
        expect(eq(metaText(meta, "serial::update_policy"), std::string("stopped")));
        expect(eq(metaText(meta, "burst_size::update_policy"), std::string("stopped")));
        expect(!meta.contains("sample_rate::update_policy"));
        expect(!meta.contains("antenna::update_policy")) << "a key that declares no policy exports none";
        expect(eq(metaText(meta, "sample_rate::unit"), std::string("S/s")));
        expect(eq(metaText(meta, "channel.*.frequency::unit"), std::string("Hz")));
        expect(meta.contains("bias_tee::visible"));
        expect(!meta.contains("sample_rate::visible")) << "a described setting exports visibility only when it declares it";
        expect(eq(metaText(meta, "bias_voltage::enabled_when_parameter"), std::string("bias_tee")));
        expect(meta.contains("bias_voltage::enabled_when_value") && meta.at("bias_voltage::enabled_when_value") == Value(true));
        expect(eq(metaText(meta, "channel.*.rssi::unit"), std::string("dBFS"))) << "a group's read-only key";
        const auto choices = meta.find("antenna::enum_values");
        expect(fatal(choices != meta.end()));
        expect(choices->second.holds<gr::Tensor<Value>>());
        expect(!meta.contains("name::description")) << "a reflected member's name is its key, and its Doc its description";
        expect(eq(metaText(meta, "name::documentation"), std::string("N.B. may not be unique -> ::unique_name"))) << "a reflected annotated member";
    };

    "describe writes a reflected block's own description and an annotated member's fields"_test = [] {
        Mixer mixer;
        gr::settings::detail::describe(mixer);
        const property_map& meta = mixer.meta_information.value;
        expect(!Mixer::description.empty());
        expect(eq(metaText(meta, "description"), std::string(Mixer::description)));
        expect(eq(metaText(meta, "sample_rate::unit"), std::string("S/s")));
        expect(eq(metaText(meta, "sample_rate::documentation"), std::string("the input's rate")));
        expect(meta.contains("sample_rate::visible") && meta.at("sample_rate::visible") == Value(true));
        expect(meta.contains("ppm::visible") == false) << "a member with no annotation exports none";
        expect(metaText(meta, "mode::enum_type").contains("Mode")) << metaText(meta, "mode::enum_type");
    };

    "describe exports each update policy as declared, and every port key as topology"_test = [] {
        Exported exported;
        gr::settings::detail::describe(exported);
        const property_map& meta = exported.meta_information.value;
        expect(eq(metaText(meta, "stopped_key::update_policy"), std::string("stopped")));
        expect(eq(metaText(meta, "n_inputs::update_policy"), std::string("topology")));
        expect(eq(metaText(meta, "n_outputs::update_policy"), std::string("topology"))) << "a port key also declared Stopped";
        expect(!meta.contains("plain_key::update_policy"));
        Radio radio;
        gr::settings::detail::describe(radio);
        const property_map& described = radio.meta_information.value;
        expect(eq(metaText(described, "n_channels::update_policy"), std::string("topology")));
        expect(!described.contains("antenna::update_policy"));
    };

    "describe exports the port collections and the limits of a reflected port key"_test = [] {
        Exported exported;
        gr::settings::detail::describe(exported);
        const property_map& meta = exported.meta_information.value;
        expect(eq(metaText(meta, "n_inputs::port_count_for"), std::string("inputs")));
        expect(meta.contains("n_inputs::minimum") && meta.at("n_inputs::minimum") == Value(std::uint32_t{1}));
        expect(meta.contains("n_inputs::maximum") && meta.at("n_inputs::maximum") == Value(std::uint32_t{8}));
        const auto both = meta.find("n_outputs::port_count_for");
        expect(fatal(both != meta.end()));
        const gr::Tensor<Value>* names = both->second.get_if<gr::Tensor<Value>>();
        expect(fatal(names != nullptr));
        expect(eq(names->size(), 2UZ));
        expect((*names)[0UZ] == Value("outputs") && (*names)[1UZ] == Value("extras"));
        expect(!meta.contains("plain_key::port_count_for"));
        Radio radio;
        gr::settings::detail::describe(radio);
        expect(eq(metaText(radio.meta_information.value, "n_channels::port_count_for"), std::string("outputs")));
        expect(radio.meta_information.value.at("sample_rate::minimum") == Value(2.0e6)) << "a described setting's limits";
    };

    "describe exports each end of a key's predicates with whether it is excluded, the values of a OneOf and their text"_test = [] {
        Bounded bounded;
        gr::settings::detail::describe(bounded);
        const property_map& meta = bounded.meta_information.value;
        expect(meta.contains("cutoff::minimum") && meta.at("cutoff::minimum") == Value(0.f));
        expect(meta.contains("cutoff::minimum_excluded") && meta.at("cutoff::minimum_excluded") == Value(true));
        expect(meta.contains("cutoff::maximum") && meta.at("cutoff::maximum") == Value(0.5f));
        expect(meta.contains("cutoff::maximum_excluded") && meta.at("cutoff::maximum_excluded") == Value(false));
        expect(eq(metaText(meta, "cutoff::limits"), std::string("> 0 and <= 0.5")));
        const auto listed = meta.find("decimation::one_of");
        expect(fatal(listed != meta.end()));
        const gr::Tensor<std::uint32_t>* values = listed->second.get_if<gr::Tensor<std::uint32_t>>();
        expect(fatal(values != nullptr));
        expect(eq(values->size(), 3UZ));
        expect((*values)[0UZ] == 1U && (*values)[1UZ] == 2U && (*values)[2UZ] == 4U);
        expect(!meta.contains("decimation::minimum") && !meta.contains("decimation::maximum"));
        expect(eq(metaText(meta, "decimation::limits"), std::string("one of: 1, 2, 4")));
        expect(meta.contains("taps::minimum") && !meta.contains("taps::maximum"));
        expect(eq(metaText(meta, "taps::limits"), std::string(">= 1")));
        const auto enumerators = meta.find("taper::one_of");
        expect(fatal(enumerators != meta.end()));
        const gr::Tensor<Value>* names = enumerators->second.get_if<gr::Tensor<Value>>();
        expect(fatal(names != nullptr));
        expect(eq(names->size(), 2UZ));
        expect((*names)[0UZ] == Value("hann") && (*names)[1UZ] == Value("hamming")) << "OneOf on an enumeration lists the names";
        expect(eq(metaText(meta, "taper::limits"), std::string("one of: hann, hamming")));
        const auto choices = meta.find("taper::enum_values");
        expect(fatal(choices != meta.end()));
        const gr::Tensor<Value>* all = choices->second.get_if<gr::Tensor<Value>>();
        expect(fatal(all != nullptr));
        expect(eq(all->size(), 3UZ)) << "the enumeration's values stay whole";
        expect(!meta.contains("taper::minimum") && !meta.contains("taper::maximum"));
        Radio radio;
        gr::settings::detail::describe(radio);
        expect(eq(metaText(radio.meta_information.value, "sample_rate::limits"), std::string(">= 2e+06 and <= 2e+07"))) << "a described setting's minimum and maximum";
    };

    "describe exports a key's access, and nothing for a read-write key"_test = [] {
        Exported exported;
        gr::settings::detail::describe(exported);
        const property_map& meta = exported.meta_information.value;
        expect(eq(metaText(meta, "level::access"), std::string("read_only")));
        expect(!meta.contains("plain_key::access"));
        Radio radio;
        gr::settings::detail::describe(radio);
        const property_map& described = radio.meta_information.value;
        expect(eq(metaText(described, "temperature::access"), std::string("read_only")));
        expect(eq(metaText(described, "channel.*.rssi::access"), std::string("read_only")));
        expect(!described.contains("serial::access"));
    };

    "describe exports every key's type, a key that holds no value included"_test = [] {
        Exported exported;
        gr::settings::detail::describe(exported);
        const property_map& meta = exported.meta_information.value;
        expect(eq(metaText(meta, "plain_key::type"), std::string("float32")));
        expect(eq(metaText(meta, "sample_count::type"), std::string(sizeof(std::size_t) == 8UZ ? "uint64" : "uint32"))) << "a std::size_t member in the platform's width";
        Radio radio;
        gr::settings::detail::describe(radio);
        expect(eq(metaText(radio.meta_information.value, "taps::type"), std::string("float32_vector"))) << "a setting with no default";
        expect(eq(metaText(radio.meta_information.value, "temperature::type"), std::string("float64")));
    };

    "the table reads and writes a setting of the struct at the block's member `settings`"_test = [] {
        StructTuner<float>      tuner;
        const DeclarationTable& table      = gr::settings::detail::declarationTable<StructTuner<float>>();
        const Declaration&      sampleRate = declarationOf(table, "sample_rate");
        expect(sampleRate.fieldAddress(&tuner) == static_cast<void*>(&tuner.settings.sample_rate));
        expect(sampleRate.writeField(&tuner, Value(2e6f)));
        expect(eq(tuner.settings.sample_rate.value, 2e6f));
        expect(sampleRate.readField(&tuner) == Value(2e6f));
        expect(declarationOf(table, "name").fieldAddress(&tuner) == static_cast<void*>(&tuner.name)) << "the base block's settings stay its direct members";
    };

    "the generated documentation lists the struct's settings"_test = [] {
        const std::string text = gr::blockDescription<StructTuner<float>>();
        expect(text.contains("sample_rate") && text.contains("level")) << text;
    };

    "a memory-resource rebinding reaches a text setting of the struct"_test = [] {
        std::pmr::monotonic_buffer_resource resource;
        StructTuner<float>                  tuner;
        tuner.rebindFieldsTo(&resource);
        expect(tuner.settings.label.value.get_allocator().resource() == &resource);
        expect(eq(std::string_view(tuner.settings.label.value), std::string_view("tuner")));
    };
};
