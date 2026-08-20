#include <benchmark.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <numeric>
#include <thread>

#include <format>

#include <gnuradio-4.0/CircularBuffer.hpp>
#include <gnuradio-4.0/HistoryBuffer.hpp>
#include <gnuradio-4.0/meta/utils.hpp>

inline const boost::ut::suite _buffer_tests = [] {
    constexpr std::size_t n_repetitions = 10;
    constexpr std::size_t samples       = 10'000'000; // minimum number of samples
    using namespace benchmark;
    using namespace gr;

    {
        CircularBuffer<int, std::dynamic_extent, ProducerType::Multi> buffer(32);
        gr::BufferWriterLike auto                                     writer = buffer.new_writer();
        gr::BufferReaderLike auto                                     reader = buffer.new_reader();

        "circular_buffer<int>(32) - multiple producer"_benchmark.repeat<n_repetitions>(samples) = [&writer, &reader] {
            static int counter = 0;
            for (std::size_t i = 0; i < samples; i++) {
                {
                    WriterSpanLike auto pSpan = writer.tryReserve(1);
                    boost::ut::expect(!pSpan.empty());
                    pSpan[0] = counter;
                    pSpan.publish(1);
                }
                ReaderSpanLike auto data = reader.get(1);
                if (data[0] != counter) {
                    throw std::runtime_error(std::format("write {} read {} mismatch", counter, data[0]));
                }
                boost::ut::expect(data.consume(1));
                counter++;
            }
        };
    }
    {
        CircularBuffer<int>   buffer(32);
        BufferWriterLike auto writer = buffer.new_writer();
        BufferReaderLike auto reader = buffer.new_reader();

        "circular_buffer<int>(32) - single producer via lambda"_benchmark.repeat<n_repetitions>(samples) = [&writer, &reader] {
            static int counter = 0;
            for (std::size_t i = 0; i < samples; i++) {
                {
                    WriterSpanLike auto pSpan = writer.tryReserve(1);
                    boost::ut::expect(!pSpan.empty());
                    pSpan[0] = counter;
                    pSpan.publish(1);
                }
                ReaderSpanLike auto data = reader.get(1);
                if (data[0] != counter) {
                    throw std::runtime_error(std::format("write {} read {} mismatch", counter, data[0]));
                }
                boost::ut::expect(data.consume(1));
                counter++;
            }
        };
    }
    {
        CircularBuffer<int>   buffer(32);
        BufferWriterLike auto writer = buffer.new_writer();
        BufferReaderLike auto reader = buffer.new_reader();

        "circular_buffer<int>(32) - single producer via reserve"_benchmark.repeat<n_repetitions>(samples) = [&writer, &reader] {
            static int counter = 0;
            for (std::size_t i = 0; i < samples; i++) {
                {
                    WriterSpanLike auto pSpan = writer.tryReserve(1LU);
                    boost::ut::expect(!pSpan.empty());
                    pSpan[0] = counter;
                    pSpan.publish(1);
                }
                ReaderSpanLike auto data = reader.get(1);
                if (data[0] != counter) {
                    throw std::runtime_error(std::format("write {} read {} mismatch", counter, data[0]));
                }
                boost::ut::expect(data.consume(1));
                counter++;
            }
        };
    }
    /*
     * left intentionally some space to improve the circular_buffer<T> implementation here
     */
    {
        HistoryBuffer<int> buffer(32);

        "history_buffer<int>(32) - push_front"_benchmark.repeat<n_repetitions>(samples) = [&buffer] {
            static int counter = 0;
            for (std::size_t i = 0; i < samples; i++) {
                buffer.push_front(counter);
                if (const auto data = buffer[0] != counter) {
                    throw std::runtime_error(std::format("write {} read {} mismatch", counter, data));
                }
                counter++;
            }
        };
        "history_buffer<int>(32) - push_back"_benchmark.repeat<n_repetitions>(samples) = [&buffer] {
            static int counter = 0;
            for (std::size_t i = 0; i < samples; i++) {
                buffer.push_back(counter);
                if (const auto data = buffer[buffer.size() - 1UZ] != counter) {
                    throw std::runtime_error(std::format("write {} read {} mismatch", counter, data));
                }
                counter++;
            }
        };
    }
    {
        HistoryBuffer<int> buffer(32);

        "history_buffer<int, 32> - push_front"_benchmark.repeat<n_repetitions>(samples) = [&buffer] {
            static int counter = 0;
            for (std::size_t i = 0; i < samples; i++) {
                buffer.push_front(counter);
                if (const auto data = buffer[0] != counter) {
                    throw std::runtime_error(std::format("write {} read {} mismatch", counter, data));
                }
                counter++;
            }
        };

        "history_buffer<int, 32> - push_back"_benchmark.repeat<n_repetitions>(samples) = [&buffer] {
            static int counter = 0;
            for (std::size_t i = 0; i < samples; i++) {
                buffer.push_back(counter);
                if (const auto data = buffer[buffer.size() - 1UZ] != counter) {
                    throw std::runtime_error(std::format("write {} read {} mismatch", counter, data));
                }
                counter++;
            }
        };
    }

    {
        CircularBuffer<int>       buffer(32);
        gr::BufferWriterLike auto writer = buffer.new_writer();
        gr::BufferReaderLike auto reader = buffer.new_reader();

        "circular_buffer<int>(32) - no checks"_benchmark.repeat<n_repetitions>(samples) = [&writer, &reader] {
            static int counter = 0;
            for (std::size_t i = 0; i < samples; i++) {
                {
                    WriterSpanLike auto pSpan = writer.tryReserve<SpanReleasePolicy::ProcessAll>(1LU);
                    boost::ut::expect(!pSpan.empty());
                    pSpan[0] = counter;
                }
                ReaderSpanLike auto         range = reader.get(1);
                [[maybe_unused]] const auto data  = range[0];
                [[maybe_unused]] const auto ret   = range.consume(1);
                counter++;
            }
        };
    }
    {
        HistoryBuffer<int, 32> buffer;

        "history_buffer<int, 32>  - no checks - push_front"_benchmark.repeat<n_repetitions>(samples) = [&buffer] {
            static int counter = 0;
            for (std::size_t i = 0; i < samples; i++) {
                buffer.push_front(counter);
                [[maybe_unused]] const auto data = buffer[0];
                counter++;
            }
        };

        "history_buffer<int, 32>  - no checks - push_back"_benchmark.repeat<n_repetitions>(samples) = [&buffer] {
            static int counter = 0;
            for (std::size_t i = 0; i < samples; i++) {
                buffer.push_back(counter);
                [[maybe_unused]] const auto data = buffer[0];
                counter++;
            }
        };
    }
    {
        HistoryBuffer<int> buffer(32);

        "history_buffer<int>(32)  - no checks - push_front"_benchmark.repeat<n_repetitions>(samples) = [&buffer] {
            static int counter = 0;
            for (std::size_t i = 0; i < samples; i++) {
                buffer.push_front(counter);
                [[maybe_unused]] const auto data = buffer[0];
                counter++;
            }
        };

        "history_buffer<int>(32)  - no checks - push_back"_benchmark.repeat<n_repetitions>(samples) = [&buffer] {
            static int counter = 0;
            for (std::size_t i = 0; i < samples; i++) {
                buffer.push_back(counter);
                [[maybe_unused]] const auto data = buffer[0];
                counter++;
            }
        };
    }
    { // FIR shape: one push per input sample, one dot product over the whole history per output sample
        constexpr std::size_t nTaps    = 289UZ;
        constexpr std::size_t nOutputs = 500'000UZ;

        std::vector<float> coefficients(nTaps);
        for (std::size_t i = 0UZ; i < nTaps; i++) {
            coefficients[i] = 1.0f / static_cast<float>(i + 1UZ);
        }

        auto dotProduct = [&coefficients](std::span<const float> window) {
            std::array<float, 8UZ> partialSums{};
            std::size_t            tap = 0UZ;
            for (; tap + partialSums.size() <= window.size(); tap += partialSums.size()) {
                for (std::size_t lane = 0UZ; lane < partialSums.size(); lane++) {
                    partialSums[lane] += window[tap + lane] * coefficients[tap + lane];
                }
            }
            for (; tap < window.size(); tap++) {
                partialSums[0] += window[tap] * coefficients[tap];
            }
            return std::reduce(partialSums.begin(), partialSums.end());
        };

        HistoryBuffer<float> history(nTaps);
        history.push_front(std::vector<float>(nTaps, 1.0f));
        float accumulator = 0.0f;

        "history_buffer<float>(289) - push_front + 289-tap dot product"_benchmark.repeat<n_repetitions>(nOutputs) = [&history, &dotProduct, &accumulator] {
            for (std::size_t i = 0UZ; i < nOutputs; i++) {
                history.push_front(static_cast<float>(i));
                accumulator += dotProduct(history.get_span(0UZ, nTaps));
            }
        };
        boost::ut::expect(accumulator != 0.0f);

        std::vector<float> flatWindow(nTaps, 1.0f);
        std::size_t        flatWritePosition = 0UZ;

        "flat window(289) - 289-tap dot product"_benchmark.repeat<n_repetitions>(nOutputs) = [&flatWindow, &flatWritePosition, &dotProduct, &accumulator] {
            for (std::size_t i = 0UZ; i < nOutputs; i++) {
                flatWindow[flatWritePosition] = static_cast<float>(i);
                flatWritePosition             = (flatWritePosition + 1UZ) < nTaps ? (flatWritePosition + 1UZ) : 0UZ;
                accumulator += dotProduct(flatWindow);
            }
        };
        boost::ut::expect(accumulator != 0.0f);

        "history_buffer<float>(289) - push_front only"_benchmark.repeat<n_repetitions>(samples) = [&history] {
            for (std::size_t i = 0UZ; i < samples; i++) {
                history.push_front(static_cast<float>(i));
            }
        };
        boost::ut::expect(history[0] != 0.0f);
    }
};

int main() { /* not needed by the UT framework */ }
