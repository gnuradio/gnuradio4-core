#ifndef GNURADIO_EXCEPTION_HPP
#define GNURADIO_EXCEPTION_HPP

#include <chrono>
#include <exception>
#include <format>
#include <print>
#include <source_location>
#include <string>
#include <string_view>

#include <gnuradio-4.0/Export.hpp>

namespace gr {

// The destructor of gr::exception is defined in the library gnuradio-shared-state. The type information and the
// virtual table of the class are emitted there, once for the whole program. A catch clause or a dynamic_cast in one
// shared object recognizes a gr::exception thrown in another, a loaded plugin included.
struct GNURADIO_EXPORT exception : public std::exception {
    std::string                           message;
    std::source_location                  sourceLocation;
    std::chrono::system_clock::time_point errorTime = std::chrono::system_clock::now();

    exception(std::string_view msg = "unknown exception", std::source_location location = std::source_location::current()) noexcept : message(msg), sourceLocation(location) {
#ifndef NDEBUG
        std::println("Exception thrown: {} at {}:{}", msg, location.file_name(), location.line());
#endif
    }

    // A user-declared destructor removes the implicit move operations. The four operations are declared as defaulted.
    exception(const exception&)            = default;
    exception(exception&&)                 = default;
    exception& operator=(const exception&) = default;
    exception& operator=(exception&&)      = default;
    ~exception() override;

    [[nodiscard]] const char* what() const noexcept override {
        if (formattedMessage.empty()) {
            formattedMessage = std::format("{} at {}:{}", message, sourceLocation.file_name(), sourceLocation.line());
        }
        return formattedMessage.c_str();
    }

private:
    mutable std::string formattedMessage; // Now storing the formatted message
};

} // namespace gr

#endif // GNURADIO_EXCEPTION_HPP
