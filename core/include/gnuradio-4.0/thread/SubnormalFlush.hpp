#ifndef GNURADIO_SUBNORMAL_FLUSH_HPP
#define GNURADIO_SUBNORMAL_FLUSH_HPP

#include <cstdint>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64) || (defined(_M_IX86) && _M_IX86_FP >= 2))
#include <float.h>

namespace gr::thread_pool::thread::detail {
using ControlWord                        = unsigned int;
inline constexpr ControlWord kFlushMask  = _MCW_DN;
inline constexpr ControlWord kFlushValue = _DN_FLUSH;

inline ControlWord readControl() noexcept {
    ControlWord control = 0U;
    _controlfp_s(&control, 0U, 0U);
    return control;
}

inline void writeControl(ControlWord control) noexcept {
    ControlWord ignored = 0U;
    _controlfp_s(&ignored, control & kFlushMask, kFlushMask);
}
} // namespace gr::thread_pool::thread::detail
#elif (defined(__x86_64__) || defined(__i386__)) && defined(__SSE2_MATH__)
#include <xmmintrin.h>

namespace gr::thread_pool::thread::detail {
using ControlWord                        = unsigned int;
inline constexpr ControlWord kFlushMask  = 0x8040U; // MXCSR bit 15 flush-to-zero, bit 6 denormals-are-zero
inline constexpr ControlWord kFlushValue = kFlushMask;

inline ControlWord readControl() noexcept { return _mm_getcsr(); }

inline void writeControl(ControlWord control) noexcept { _mm_setcsr(control); }
} // namespace gr::thread_pool::thread::detail
#elif defined(__aarch64__)
namespace gr::thread_pool::thread::detail {
using ControlWord                        = std::uint64_t;
inline constexpr ControlWord kFlushMask  = ControlWord{1U} << 24U; // FPCR bit 24 FZ, for operands and results
inline constexpr ControlWord kFlushValue = kFlushMask;

inline ControlWord readControl() noexcept {
    ControlWord control = 0U;
    __asm__ volatile("mrs %0, fpcr" : "=r"(control));
    return control;
}

inline void writeControl(ControlWord control) noexcept { __asm__ volatile("msr fpcr, %0" : : "r"(control)); }
} // namespace gr::thread_pool::thread::detail
#else
namespace gr::thread_pool::thread::detail {
using ControlWord                        = unsigned int;
inline constexpr ControlWord kFlushMask  = 0U;
inline constexpr ControlWord kFlushValue = 0U;

inline ControlWord readControl() noexcept { return 0U; }

inline void writeControl(ControlWord) noexcept {}
} // namespace gr::thread_pool::thread::detail
#endif

/*
 * Flush-to-zero and denormals-are-zero for the calling thread.
 *
 * Each thread holds its own floating-point control register. With the flush on, an operation whose float or double
 * result would be subnormal returns zero, and a subnormal operand reads as zero. Without it, many x86 cores take a
 * microcode assist for each subnormal operand or result, which costs a block on a near-silent signal far more than
 * the arithmetic.
 *
 * On x86 with SSE2 floating-point math the flush sets the MXCSR bits FTZ and DAZ, on AArch64 the FPCR bit FZ, and under
 * MSVC (x64, ARM64, and x86 with /arch:SSE2 or above) the _DN_FLUSH denormal control. kCanFlushSubnormals is false on
 * every other target, x87 floating-point math and WebAssembly included. There the functions change nothing and
 * subnormalsFlushed() returns false.
 */
namespace gr::thread_pool::thread {

inline constexpr bool kCanFlushSubnormals = detail::kFlushMask != 0U;

[[nodiscard]] inline bool subnormalsFlushed() noexcept {
    if constexpr (kCanFlushSubnormals) {
        return (detail::readControl() & detail::kFlushMask) == detail::kFlushValue;
    } else {
        return false;
    }
}

inline void setSubnormalFlush(bool flush) noexcept {
    if constexpr (kCanFlushSubnormals) {
        detail::writeControl((detail::readControl() & ~detail::kFlushMask) | (flush ? detail::kFlushValue : detail::ControlWord{0U}));
    }
}

/**
 * @brief Sets the calling thread's flush for the lifetime of the scope.
 *
 * The destructor gives the thread back the flush bits it had at construction and leaves the rest of the control
 * register as the code inside the scope left it.
 */
class SubnormalFlushScope {
    detail::ControlWord _previousFlushBits = detail::readControl() & detail::kFlushMask;

public:
    explicit SubnormalFlushScope(bool flush) noexcept { setSubnormalFlush(flush); }

    ~SubnormalFlushScope() {
        if constexpr (kCanFlushSubnormals) {
            detail::writeControl((detail::readControl() & ~detail::kFlushMask) | _previousFlushBits);
        }
    }

    SubnormalFlushScope(const SubnormalFlushScope&)            = delete;
    SubnormalFlushScope(SubnormalFlushScope&&)                 = delete;
    SubnormalFlushScope& operator=(const SubnormalFlushScope&) = delete;
    SubnormalFlushScope& operator=(SubnormalFlushScope&&)      = delete;
};

} // namespace gr::thread_pool::thread

#endif // GNURADIO_SUBNORMAL_FLUSH_HPP
