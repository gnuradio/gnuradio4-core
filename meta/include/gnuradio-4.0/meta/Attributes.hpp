#ifndef GNURADIO_META_ATTRIBUTES_HPP
#define GNURADIO_META_ATTRIBUTES_HPP

// Portable spellings for attributes whose standard form not every compiler honours.

// MSVC ignores the standard attribute for ABI-compatibility reasons and honours only its own spelling.
#ifdef _MSC_VER
#define GR_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#define GR_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

// N.B. deliberately distinct from the `forceinline` macro that AtomicRef.hpp & friends define and later `#undef`.
#ifdef _MSC_VER
#define GR_ALWAYS_INLINE __forceinline
#else
#define GR_ALWAYS_INLINE [[gnu::always_inline]]
#endif

#endif // GNURADIO_META_ATTRIBUTES_HPP
