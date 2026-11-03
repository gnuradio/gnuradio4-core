#ifndef GNURADIO_MAGIC_ENUM_HPP
#define GNURADIO_MAGIC_ENUM_HPP

#ifdef __GNUC__
#pragma GCC diagnostic push // ignore warnings from external lib we can't control
#ifndef __clang__
#pragma GCC diagnostic ignored "-Wuseless-cast"
#endif
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif

// magic_enum releases from 0.9.7 install their headers below magic_enum/, earlier ones directly into the include directory
#if __has_include(<magic_enum/magic_enum.hpp>)
#include <magic_enum/magic_enum.hpp>
#include <magic_enum/magic_enum_utility.hpp>
#else
#include <magic_enum.hpp>
#include <magic_enum_utility.hpp>
#endif

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#endif // GNURADIO_MAGIC_ENUM_HPP
