#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/tools/RunGraph.hpp>

#include "good_base_plugin.hpp"

/**
 * @brief gr::tools::runGraph called twice in one process, with core's test blocks linked into the test.
 *
 * The first call runs an endless graph until SIGINT stops it. The second runs a graph that ends on its own. The case
 * pins that the first call puts back the handlers it replaced, and that its signal does not stop the second call.
 */

namespace {

constexpr std::string_view kGraphFile{GR_TOOLS_TEST_ASSETS "/source_to_sink.yaml"};

using SignalHandler = decltype(std::signal(SIGINT, SIG_DFL));

extern "C" {
static void onHostSignal(int) {}
}

/// standard error redirected into a file for the duration of one call
struct StderrCapture {
    std::filesystem::path path = std::filesystem::temp_directory_path() / std::format("qa_RunGraphLibrary.{}.err", ::getpid());

    int _savedFd = -1;

    StderrCapture() {
        std::fflush(stderr);
        _savedFd            = ::dup(STDERR_FILENO);
        const int captureFd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        ::dup2(captureFd, STDERR_FILENO);
        ::close(captureFd);
    }

    StderrCapture(const StderrCapture&)            = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;

    ~StderrCapture() {
        std::fflush(stderr);
        ::dup2(_savedFd, STDERR_FILENO);
        ::close(_savedFd);
        std::filesystem::remove(path);
    }

    [[nodiscard]] std::string text() const {
        std::fflush(stderr);
        std::ifstream     file(path);
        std::stringstream captured;
        captured << file.rdbuf();
        return captured.str();
    }
};

struct Call {
    int         exitCode = -1;
    std::string output;
};

[[nodiscard]] Call callRunGraph(const std::vector<std::string>& arguments) {
    std::vector<const char*> argv{"rungraph"};
    for (const std::string& argument : arguments) {
        argv.push_back(argument.c_str());
    }
    const StderrCapture capture;
    const int           exitCode = gr::tools::runGraph(static_cast<int>(argv.size()), argv.data());
    return {.exitCode = exitCode, .output = capture.text()};
}

} // namespace

const boost::ut::suite<"rungraph library"> runGraphLibraryTests = [] {
    using namespace boost::ut;

    "a second call is unaffected by the signal that stopped the first, and finds the handlers it replaced"_test = [] {
        gr::BlockRegistry& registry = gr::globalBlockRegistry();
        expect(registry.insert<good::fixed_source<float>>());
        expect(registry.insert<good::cout_sink<float>>());
        std::signal(SIGINT, onHostSignal);
        std::signal(SIGTERM, onHostSignal);

        // SIGINT is raised until the call returns. A signal that comes before the call sets its own handler reaches
        // onHostSignal.
        std::atomic<bool> firstReturned{false};
        std::thread       interrupter([&firstReturned] {
            while (!firstReturned.load()) {
                std::raise(SIGINT);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
        const Call        first = callRunGraph({"--graph", std::string(kGraphFile)});
        firstReturned.store(true);
        interrupter.join();
        expect(eq(first.exitCode, 0)) << first.output;
        expect(first.output.contains("rungraph: the graph was stopped before it ended")) << "SIGINT stopped the endless graph" << first.output;

        const SignalHandler interruptAfter = std::signal(SIGINT, onHostSignal);
        const SignalHandler terminateAfter = std::signal(SIGTERM, onHostSignal);
        expect(interruptAfter == onHostSignal) << "the SIGINT handler the call replaced is back";
        expect(terminateAfter == onHostSignal) << "the SIGTERM handler the call replaced is back";

        const Call second = callRunGraph({"--graph", std::string(kGraphFile), "--set", "source.event_count=100"});
        expect(eq(second.exitCode, 0)) << second.output;
        expect(second.output.contains("rungraph: the graph ended on its own")) << "the signal to the first call does not stop the second" << second.output;
    };
};

int main() { /* not needed for UT */ }
