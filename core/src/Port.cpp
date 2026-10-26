#include <gnuradio-4.0/Port.hpp>

namespace gr {

PortMetaInfo::PortMetaInfo(const property_map& metaInfo) noexcept(false) {
    if (auto res = update(metaInfo); !res.has_value()) {
        throw gr::exception(res.error().message, res.error().sourceLocation);
    }
}

void PortMetaInfo::reset() { auto_update = defaultAutoUpdate(); }

namespace {
// sets each field that fieldNameOf maps an entry's key to, when auto_update holds the field's name
template<typename TFieldNameOf>
std::expected<void, Error> updateFields(PortMetaInfo& metaInfo, const property_map& entries, TFieldNameOf fieldNameOf, const std::source_location location) noexcept {
    std::expected<void, Error> maybeError = {};
    for (const auto& [key, value] : entries) {
        const std::string_view fieldKey = fieldNameOf(std::string_view(key));
        if (fieldKey.empty() || !metaInfo.auto_update.contains(fieldKey)) {
            continue;
        }
        refl::for_each_data_member_index<PortMetaInfo>([&key, fieldKey, &value, &maybeError, &location, &metaInfo](auto kIdx) {
            using MemberType = refl::data_member_type<PortMetaInfo, kIdx>;
            using Type       = unwrap_if_wrapped_t<std::remove_cvref_t<MemberType>>;

            const auto fieldName = refl::data_member_name<PortMetaInfo, kIdx>.view();
            if (fieldName == fieldKey) {
                auto& member = refl::data_member<kIdx>(metaInfo);
                if constexpr (std::is_same_v<Type, std::string>) {
                    const auto str = value.value_or(std::string_view{});
                    if (str.data()) {
                        std::ignore = member.validate_and_set(std::string(str));
                    } else {
                        maybeError = std::unexpected(Error{std::format("PortMetaInfo invalid-argument: incorrect type for key")});
                    }
                } else {
                    const auto converted = pmt::convert_numerically<Type>(value);
                    if (converted) {
                        std::ignore = member.validate_and_set(*converted);
                    } else {
                        maybeError = std::unexpected(Error{std::format("PortMetaInfo invalid-argument: incorrect type for key {} (expected:{}, got:{} {}, value:{})", //
                                                               std::string_view(key), gr::meta::type_name<Type>(), value.value_type(), value.container_type(), value),
                            location});
                    }
                }
            }
        });
    }

    if (!maybeError.has_value()) {
        return maybeError;
    }
    return {};
}
} // namespace

std::expected<void, Error> PortMetaInfo::update(const property_map& metaInfo, const std::source_location location) noexcept {
    return updateFields(*this, metaInfo, [](std::string_view key) { return key; }, location);
}

std::expected<void, Error> PortMetaInfo::updateFromTag(const property_map& tagMap, const std::source_location location) noexcept {
    return updateFields(*this, tagMap, [](std::string_view key) { return gr::tag::domainOf(key) == gr::tag::kGrDomain ? gr::tag::nameOf(key) : std::string_view{}; }, location);
}

property_map PortMetaInfo::get() const noexcept {
    property_map metaInfo;
    refl::for_each_data_member_index<PortMetaInfo>([&metaInfo, this](auto kIdx) { //
        metaInfo.insert_or_assign(std::pmr::string(refl::data_member_name<PortMetaInfo, kIdx>.view()), refl::data_member<kIdx>(*this).value);
    });

    return metaInfo;
}

} // namespace gr
