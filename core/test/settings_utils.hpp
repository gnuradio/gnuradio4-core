#ifndef CORE_TEST_SETTINGS_UTILS_HPP
#define CORE_TEST_SETTINGS_UTILS_HPP

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <ios>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <thread>

#include <gnuradio-4.0/Settings.hpp>

namespace gr::testing {

// a test's stand-in for a block's work call: applies the pending changes and returns the failed keys
inline gr::settings::Rejected applyStagedForTest(gr::SettingsBase& settings) { return gr::settings::detail::FrameworkAccess::applyStagedParameters(settings); }

// polls `condition` every millisecond until it holds or 10 s pass, and returns whether it held
[[nodiscard]] inline bool waitUntil(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// a call's result as text: the refusals, or each key with its state and reason
[[nodiscard]] inline std::string textOf(const std::expected<gr::settings::Values, gr::settings::Rejected>& result) {
    if (!result) {
        return gr::settings::describeRejected(result.error());
    }
    std::string text;
    for (const auto& [key, entry] : *result) {
        text += std::format("{} ({}: {}) ", key, static_cast<int>(entry.state), entry.reason);
    }
    return text.empty() ? std::string("no entries") : text;
}

// each key of a read that holds a value and that a call may set: a read holds the read-only keys, such as every
// block's unique name, and a call that names one is refused
template<typename TSettings>
[[nodiscard]] gr::property_map writableValues(const TSettings& settings, const std::expected<gr::settings::Values, gr::settings::Rejected>& read) {
    gr::property_map            values   = gr::settings::heldValues(read);
    const std::set<std::string> writable = settings.writableMembers();
    std::erase_if(values, [&writable](const auto& entry) { return !writable.contains(std::string(entry.first)); });
    return values;
}

// each key sent with its outcome: failed with the refusal's text, its entry, or the value sent when it is in force as sent
[[nodiscard]] inline gr::settings::Values everyKeySent(const gr::property_map& sent, const std::expected<gr::settings::Values, gr::settings::Rejected>& result) {
    using gr::settings::Value;
    using gr::settings::Values;
    Values outcome;
    if (!result.has_value()) {
        const std::string refusal = gr::settings::describeRejected(result.error());
        for (const auto& [key, _] : sent) {
            outcome.insert_or_assign(std::string(key), Value{.state = Value::State::failed, .value = {}, .reason = refusal});
        }
        return outcome;
    }
    for (const auto& [key, value] : sent) {
        const auto it = result->find(std::string_view(key));
        outcome.insert_or_assign(std::string(key), it != result->end() ? it->second : Value{.state = Value::State::value, .value = value, .reason = {}});
    }
    return outcome;
}

/**
 * Redirects the standard error stream, where the default reader prints, for the life of the object.
 *
 * The buffer keeps no put area, so each write reaches it under its lock, and a read of the text while a worker prints
 * is ordered against the write. Swapping the stream's buffer is not: construct and destroy the object while no worker
 * of the case runs.
 */
class CerrCapture : public std::streambuf {
    mutable std::mutex _lock;
    std::string        _text;
    std::streambuf*    _previous = std::cerr.rdbuf(this);

protected:
    int_type overflow(int_type character) override {
        if (!traits_type::eq_int_type(character, traits_type::eof())) {
            std::lock_guard guard(_lock);
            _text.push_back(traits_type::to_char_type(character));
        }
        return traits_type::not_eof(character);
    }

    std::streamsize xsputn(const char_type* characters, std::streamsize count) override {
        std::lock_guard guard(_lock);
        _text.append(characters, static_cast<std::string::size_type>(count));
        return count;
    }

public:
    CerrCapture()                              = default;
    CerrCapture(const CerrCapture&)            = delete;
    CerrCapture& operator=(const CerrCapture&) = delete;
    ~CerrCapture() override {
        try {
            std::cerr.rdbuf(_previous);
        } catch (...) { // rdbuf() throws through clear() when the previous buffer is null and cerr has an exception mask; a destructor must not throw
        }
    }

    [[nodiscard]] std::string str() const {
        std::lock_guard guard(_lock);
        return _text;
    }

    // the lines of the text that hold `containing`
    [[nodiscard]] std::size_t lines(std::string_view containing) const {
        std::istringstream all(str());
        std::size_t        count = 0UZ;
        for (std::string line; std::getline(all, line);) {
            count += line.contains(containing) ? 1UZ : 0UZ;
        }
        return count;
    }
};

} // namespace gr::testing

#endif // CORE_TEST_SETTINGS_UTILS_HPP
