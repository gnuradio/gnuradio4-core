#ifndef GR_BLOCKLIB_FIXTURE_MARKERS_HPP
#define GR_BLOCKLIB_FIXTURE_MARKERS_HPP

// A fixture for the generator, not for the compiler: it carries one of every marker shape the
// generator has to parse. Nothing here is instantiated, and the file is never compiled.

namespace gr::fixture {

// an unbalanced bracket in ordinary source, which the line scanner must step over without losing
// the line numbers reported below: '{}[{}]' outside of [0, {}[

GR_REGISTER_BLOCK(gr::fixture::Bare)

GR_REGISTER_BLOCK(gr::fixture::Scale, [T], [ float, std::complex<float> ])

GR_REGISTER_BLOCK("NamedBlock", gr::fixture::Named, [T], [float])

GR_REGISTER_BLOCK(gr::fixture::Fixed, ([T], 1UZ), [ float, double ])

GR_REGISTER_BLOCK(gr::fixture::Combine, ([T], std::plus<[T]>), [float])

GR_REGISTER_BLOCK(gr::fixture::Pair, ([T], [U]), [ float, double ], [ int, short ])

// clang-format off
// two literals with one character between them
GR_REGISTER_BLOCK(gr::fixture::Values, ([T], 1UZ,2UZ, 3ul, 1.0L, 1.5E+3L, 0x10u, int16_t), [float])
// clang-format on

GR_REGISTER_BLOCK("NamedFixed", gr::fixture::Fixed, ([T], 3UZ), [float])

GR_REGISTER_BLOCK(gr::fixture::Labeled, ([T], "rate 2u"), [float])

// GR_REGISTER_BLOCK(gr::fixture::CommentedOut, [T], [float])

} // namespace gr::fixture

#endif // GR_BLOCKLIB_FIXTURE_MARKERS_HPP
