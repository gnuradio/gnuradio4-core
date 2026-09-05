#ifndef GNURADIO_BLOCK_REGISTRATION_HPP
#define GNURADIO_BLOCK_REGISTRATION_HPP

/**
 * @brief The narrow face of block registration: everything needed to add an entry, nothing that
 * names a block type.
 *
 * A registry entry is a key, an alias and a factory pointer, so producing one must not instantiate
 * the block. The generated registration units include this header alone -- `BlockRegistration` stays
 * incomplete here and is only ever passed on by reference -- so they instantiate no `Block<T>`,
 * `BlockWrapper<T>`, `CtxSettings<T>` or `Port<T, ...>`. The materialisation lives in the generated
 * definition unit that defines the factory, one per registered (block, type), which is the only
 * place `BlockRegistry.hpp` and the block header are parsed.
 *
 * `BlockRegistry.hpp` defines `BlockRegistration` and `makeBlockRegistration<TBlock>()`, which the
 * definition unit uses to derive the key and the alias exactly as the `insert<TBlock>()` path does.
 */

#include <gnuradio-4.0/config.hpp>

#include <gnuradio-4.0/Export.hpp>

#include <source_location>

namespace gr {

class BlockRegistry;
struct BlockRegistration;

GNURADIO_EXPORT
BlockRegistry& globalBlockRegistry(std::source_location location = std::source_location::current());

/// Adds `registration` under its key and, when it has one, its alias. Returns whether a key was added.
GNURADIO_EXPORT
bool insertBlockFactory(BlockRegistry& registry, const BlockRegistration& registration);

} // namespace gr

/// Defined by GR_PLUGIN: the plugin's own block registry, reachable without the plugin's type.
GNURADIO_EXPORT
gr::BlockRegistry& grPluginBlockRegistry();

#endif // GNURADIO_BLOCK_REGISTRATION_HPP
