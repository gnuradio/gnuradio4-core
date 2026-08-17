#!/usr/bin/env python3
"""Generate the block-registration translation units for one header.

Run once per header while a block library configures:

    parse_registrations.py --header <header.hpp> --out-dir <generation dir>
                           [--split] [--max-per-tu <n>]
                           [--registry-header <include>] [--registry-instance <expression>]

This is the same generator as GrParseRegistrations.cmake beside it, in Python, and it writes the
same files with the same contents; the block-library macros run whichever of the two the configure
selected. See that script for what the generated units are and what the switches group.

Only the standard library is used, so the interpreter's presence is the whole dependency.
"""

import argparse
import os
import sys

MACRO_NAME = "GR_REGISTER_BLOCK"

# the placeholders a parameter pack may carry, in the order the expansion groups fill them
PLACEHOLDERS = ("[T]", "[U]", "[A]", "[B]", "[X]", "[Y]", "[Z]", "[S]")

OPENING = "([<"
CLOSING = ")]>"


INTEGRATOR_SOURCE_TEMPLATE = """
            #include <gnuradio-4.0/BlockRegistry.hpp>

            #include "declarations.hpp"

            extern "C" {
                GNURADIO_EXPORT
                std::size_t gr_blocklib_init_module_@MODULE@(gr::BlockRegistry& registry) {
                    std::size_t result = 0UZ;
                    #include "raw_calls.hpp"
                    return result;
                }
            }
"""

INTEGRATOR_HEADER_TEMPLATE = """
            #ifndef GR_BLOCKLIB_INIT_MODULE_@MODULE@
            #define GR_BLOCKLIB_INIT_MODULE_@MODULE@
            namespace gr { class BlockRegistry; }

            extern "C" {
                GNURADIO_EXPORT
                std::size_t gr_blocklib_init_module_@MODULE@(gr::BlockRegistry& registry);
            }

            namespace gr::blocklib {
                inline
                std::size_t init@MODULE@(gr::BlockRegistry& registry) {
                    return gr_blocklib_init_module_@MODULE@(registry);
                }
            }
            #endif
"""


class ParseError(Exception):
    """A marker the generator cannot read; the message names the header and the line."""


def split_top_level(text):
    """Split at the commas that are not inside brackets, angle brackets or a string literal."""
    tokens = []
    token = []
    depth = 0
    in_string = False
    for char in text.strip():
        if in_string:
            token.append(char)
            if char == '"':
                in_string = False
            continue
        if char == '"':
            in_string = True
        elif char in OPENING:
            depth += 1
        elif char in CLOSING:
            depth -= 1
            if depth < 0:
                raise ParseError("mismatched bracket")
        elif char == "," and depth == 0:
            tokens.append("".join(token).strip())
            token = []
            continue
        token.append(char)
    if depth != 0 or in_string:
        raise ParseError("mismatched bracket")
    tokens.append("".join(token).strip())
    return tokens


def replace_placeholders(param_pack, values):
    """Substitute the expansion values for the placeholders, the outer parentheses dropped."""
    param = param_pack.lstrip("( ").rstrip(") ")
    for placeholder, value in zip(PLACEHOLDERS, values):
        param = param.replace(placeholder, value)
    return param


def cartesian_product(groups):
    """The combinations of one value per group, the last group varying fastest."""
    combinations = [()]
    for group in groups:
        combinations = [prefix + (value,) for prefix in combinations for value in group]
    return combinations


def parse_marker(body):
    """Read one GR_REGISTER_BLOCK body into its override name, block type, pack and groups."""
    parts = split_top_level(body)
    index = 0
    base_name = ""
    if len(parts[0]) >= 2 and parts[0].startswith('"') and parts[0].endswith('"'):
        base_name = parts[0][1:-1]
        index = 1

    if index >= len(parts):
        raise ParseError("missing the block type argument")
    template_name = parts[index]
    index += 1

    param_pack = ""
    if index < len(parts):
        param_pack = parts[index]
        index += 1

    groups = []
    for chunk in parts[index:]:
        if len(chunk) >= 2 and chunk.startswith("[") and chunk.endswith("]"):
            chunk = chunk[1:-1].strip()
        group = [value for value in split_top_level(chunk) if value]
        if group:
            groups.append(group)
    return base_name, template_name, param_pack, groups


def read_lines(path):
    """The header's lines, without their line endings, in source order."""
    with open(
        path, "r", encoding="utf-8", errors="surrogateescape", newline=""
    ) as source:
        return source.read().split("\n")


def registrations_of(header, lines):
    """Every (block, type) the header registers, in source order."""
    pending = []
    macro_count = 0
    for line_number, raw in enumerate(lines, start=1):
        line = raw.rstrip("\r").strip()
        if not line or line.startswith("//") or MACRO_NAME not in line:
            continue

        print("\tfound macro on line {}: '{}'".format(line_number, line))

        after_macro = line.index(MACRO_NAME) + len(MACRO_NAME)
        opening = line.find("(", after_macro)
        if opening < 0:
            raise ParseError(
                "{}:{}: missing '(' after {}".format(header, line_number, MACRO_NAME)
            )
        closing = line.rfind(")")
        if closing <= opening + 1:
            raise ParseError(
                "{}:{}: missing ')' after {}".format(header, line_number, MACRO_NAME)
            )

        try:
            base_name, template_name, param_pack, groups = parse_marker(
                line[opening + 1 : closing]
            )
        except ParseError as error:
            raise ParseError("{}:{}: {}".format(header, line_number, error)) from error

        for values in cartesian_product(groups):
            replaced = replace_placeholders(param_pack, values)
            final_name = (
                "{}<{}>".format(base_name, replaced)
                if base_name and replaced
                else base_name
            )
            block_type = (
                "{}<{}>".format(template_name, replaced) if replaced else template_name
            )
            pending.append((block_type, final_name, line_number, macro_count))
        macro_count += 1
    return pending, macro_count


def registration_units(stem, pending, split, max_per_tu):
    """Group the registrations into the declaration-only units that carry them."""
    units = []
    if max_per_tu > 0 and pending:
        total = len(pending)
        chunk_count = (total + max_per_tu - 1) // max_per_tu
        next_index = 0
        for chunk in range(chunk_count):
            # sizes differ by at most one, none exceeding max_per_tu
            size = total // chunk_count + (1 if chunk < total % chunk_count else 0)
            units.append(
                (
                    "{}_{}".format(stem, chunk),
                    list(range(next_index, next_index + size)),
                )
            )
            next_index += size
        return units

    local_index = 0
    for index, entry in enumerate(pending):
        macro_index = entry[3]
        local_index = (
            local_index + 1 if index > 0 and pending[index - 1][3] == macro_index else 0
        )
        if split:
            units.append(("{}_{}_{}".format(stem, macro_index, local_index), [index]))
        elif local_index == 0:
            units.append(("{}_{}".format(stem, macro_index), [index]))
        else:
            units[-1][1].append(index)
    return units


def write_file(path, text):
    print("\t=> Generating file: '{}'".format(path))
    with open(
        path, "w", encoding="utf-8", errors="surrogateescape", newline=""
    ) as target:
        target.write(text)


def main(argv=None):
    parser = argparse.ArgumentParser(add_help=True, description=__doc__)
    parser.add_argument("--header", required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--split", action="store_true")
    parser.add_argument("--max-per-tu", type=int, default=0)
    parser.add_argument("--registry-header", default=None)
    parser.add_argument("--registry-instance", default="gr::globalBlockRegistry")
    options = parser.parse_args(argv)

    header = options.header
    out_dir = options.out_dir
    if not os.path.exists(header):
        raise ParseError("'{}' not found".format(header))
    if options.split and options.max_per_tu > 0:
        raise ParseError("--split and --max-per-tu are mutually exclusive")

    registry_header = options.registry_header or "gnuradio-4.0/BlockRegistry.hpp"
    registry_instance = options.registry_instance

    generator = os.path.abspath(__file__)
    stem = os.path.basename(header).split(".", 1)[0]
    module = os.path.basename(os.path.normpath(out_dir))
    os.makedirs(out_dir, exist_ok=True)

    print(
        "parsing header: '{}' -> '{}'  split: {}  max-per-tu: {}".format(
            header, out_dir, "Yes" if options.split else "No", options.max_per_tu
        )
    )

    # the two module-wide files are written once; a second header of the same module leaves them alone.
    # Literal braces are everywhere in these two, so the module name is substituted rather than formatted.
    integrator_source = os.path.join(out_dir, "integrator.cpp")
    if not os.path.exists(integrator_source):
        write_file(
            integrator_source,
            INTEGRATOR_SOURCE_TEMPLATE.replace("@MODULE@", module),
        )

    integrator_header = os.path.join(out_dir, module + ".hpp")
    if not os.path.exists(integrator_header):
        write_file(
            integrator_header,
            INTEGRATOR_HEADER_TEMPLATE.replace("@MODULE@", module),
        )

    pending, macro_count = registrations_of(header, read_lines(header))
    file_count = 0

    # declaration and definition in one: the unit names the block type, so it materialises it
    for unit_name, members in registration_units(
        stem, pending, options.split, options.max_per_tu
    ):
        init = "gr_blocklib_init_unit_{}".format(unit_name)

        text = "// auto-generated by {}, do not edit.\n".format(generator)
        text += '#include <{}>\n#include "{}" // for details: {}:1\n\n'.format(
            registry_header, header, header
        )

        functions = []
        for position, member in enumerate(members):
            function = "reg_{}_{}".format(unit_name, position)
            functions.append(function)
            text += (
                "\n    namespace gr {{ class BlockRegistry; }}\n"
                "    namespace {{\n"
                "        bool {function}(gr::BlockRegistry& registry) {{\n"
                '            return gr::registerBlock<{block_type}, "{final_name}">(registry);'
                ' // for details: {instance}:{header}\\n"\n'
                "        }}\n"
                "    }}\n"
            ).format(
                function=function,
                block_type=pending[member][0],
                final_name=pending[member][1],
                instance=registry_instance,
                header=header,
            )

        text += '\nextern "C" {{\nGNURADIO_EXPORT std::size_t {}(gr::BlockRegistry& registry) {{\n'.format(
            init
        )
        text += "    std::size_t result = 0UZ; \n"
        for function in functions:
            text += "    result += ( !{}(registry) ? 1UZ : 0UZ );\n".format(function)
        text += "    return result;\n}\n}\n\n"
        text += "auto {init}_invoked = {init}({instance}());\n".format(
            init=init, instance=registry_instance
        )
        text += "// To initialize, call {}\n".format(init)
        text += "// end of auto-generated code\n"

        write_file(os.path.join(out_dir, unit_name + ".cpp"), text)
        write_file(
            os.path.join(out_dir, unit_name + "_declarations.hpp.in"),
            "#ifndef HEADER_GUARD_{init}_HPP\n"
            "#define HEADER_GUARD_{init}_HPP\n"
            'extern "C" {{ std::size_t {init}(gr::BlockRegistry&); }}\n'
            "#endif // HEADER_GUARD_{init}_HPP\n".format(init=init),
        )
        write_file(
            os.path.join(out_dir, unit_name + "_raw_calls.hpp.in"),
            "result += !{}(registry);\n".format(init),
        )
        print("\t=> To initialize, call {}".format(init))
        file_count += 1

    print(
        "parse_registrations: wrote {} file(s) for {} macro definition(s).".format(
            file_count, macro_count
        )
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ParseError as error:
        sys.stderr.write("parse_registrations: error: {}\n".format(error))
        sys.exit(1)
