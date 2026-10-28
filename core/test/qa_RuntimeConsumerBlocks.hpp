#ifndef QA_RUNTIME_CONSUMER_BLOCKS_HPP
#define QA_RUNTIME_CONSUMER_BLOCKS_HPP

#include <cstddef>

/// Declared without a single GNU Radio include. The consumer half then stays on Runtime.hpp alone.
namespace qa_consumer {

bool registerDemoBlocks();

void addToTotal(double sum, std::size_t nSamples);

[[nodiscard]] double      total();
[[nodiscard]] std::size_t totalSamples();

} // namespace qa_consumer

#endif // QA_RUNTIME_CONSUMER_BLOCKS_HPP
