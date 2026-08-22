#include <boost/ut.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

/**
 * @brief The diagnostic for a block that reports success without making progress.
 *
 * A processBulk() that returns OK while consuming and publishing nothing leaves the scheduler with no reason to stop
 * and no reason to wait, so the graph spins at full speed and produces nothing. The framework does not break that
 * cycle - it only names the block once the cycle has lasted about a second.
 * The blocks below are three shapes a work call can take: stuck, out of output space, and out of
 * input, and only the first of them may be reported.
 */

namespace qa_block_diagnostics {

using namespace gr;

inline constexpr auto kReportDeadline = std::chrono::seconds(4);

/// stderr redirected into a file for the duration of a run, so that the run's own diagnostics can be read back
struct StderrCapture {
    std::filesystem::path path = std::filesystem::temp_directory_path() / std::format("qa_BlockDiagnostics.{}.err", ::getpid());

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

struct EndlessSource : Block<EndlessSource> {
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(EndlessSource, out);

    work::Status processBulk(OutputSpanLike auto& outSpan) {
        const std::size_t n = outSpan.size();
        if (n == 0UZ) {
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = 1.f;
        }
        outSpan.publish(n);
        return work::Status::OK;
    }
};

/// the defect under diagnosis: success reported for a work call that touched neither side
struct NeverProgresses : Block<NeverProgresses> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(NeverProgresses, in, out);

    std::size_t nWorkCalls = 0UZ;

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        nWorkCalls++;
        std::ignore = inSpan.consume(0UZ);
        outSpan.publish(0UZ);
        return work::Status::OK;
    }
};

struct DiscardingSink : Block<DiscardingSink> {
    PortIn<float> in;

    GR_MAKE_REFLECTABLE(DiscardingSink, in);

    std::size_t nWorkCalls = 0UZ;

    work::Status processBulk(InputSpanLike auto& inSpan) {
        nWorkCalls++;
        std::ignore = inSpan.consume(inSpan.size());
        return work::Status::OK;
    }
};

} // namespace qa_block_diagnostics

const boost::ut::suite<"block diagnostics"> _blockDiagnostics = [] {
    using namespace boost::ut;
    using namespace qa_block_diagnostics;

    "a block that returns OK without progress is named on stderr, once, while the graph keeps spinning"_test = [] {
        StderrCapture capture;

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<EndlessSource>();
        auto&     stuck  = flow.emplaceBlock<NeverProgresses>();
        auto&     sink   = flow.emplaceBlock<DiscardingSink>();
        expect(flow.connect<"out", "in">(source, stuck).has_value());
        expect(flow.connect<"out", "in">(stuck, sink).has_value());

        gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{};
        expect(scheduler.exchange(std::move(flow)).has_value());

        std::jthread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });

        const auto  deadline = std::chrono::steady_clock::now() + kReportDeadline;
        std::string reported;
        while (reported.empty() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            reported = capture.text();
        }
        const bool keptSpinning = gr::lifecycle::isActive(scheduler.state());

        scheduler.requestStop();
        runner.join();

        expect(!reported.empty()) << "a graph stuck for a second must be reported";
        expect(reported.contains(std::string_view(stuck.unique_name))) << std::format("the report must name the stuck block, got: {}", reported);
        expect(!reported.contains(std::string_view(source.unique_name))) << "INSUFFICIENT_OUTPUT_ITEMS is not a zero-progress OK";
        expect(!reported.contains(std::string_view(sink.unique_name))) << "a block the framework never dispatches is not stuck";
        expect(eq(static_cast<std::size_t>(std::ranges::count(reported, '\n')), 1UZ)) << std::format("one line per stuck episode, not one per work call, got: {}", reported);

        expect(keptSpinning) << "diagnose only: the report must not stop the graph";
        expect(gt(stuck.nWorkCalls, 1UZ)) << "the block must be dispatched throughout, not parked";
        expect(eq(sink.nWorkCalls, 0UZ)) << "a sink without input is never dispatched, so it cannot be reported";
    };
};

int main() { /* not needed by the UT framework */ }
