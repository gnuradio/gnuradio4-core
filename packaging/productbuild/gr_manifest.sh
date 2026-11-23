#!/bin/sh
#
# Reads and maintains the shared GNU Radio 4 install manifest.
#
# Every GNU Radio 4 installer (gr-core, gr-blocks, ... and the combined
# superbuild installer) shares one manifest per plugin API version.  It records
# where the runtime libraries and the plugins live, plus the version of every
# component and plugin that is currently installed.
#
# This is the macOS counterpart of packaging/innosetup/gr_manifest.ps1.  The two
# write the same file: the JSON schema, the '|'-separated 'name=version' inputs
# and the 'key=value' output vocabulary are identical, so a reader never has to
# care which platform produced the manifest.  Keep them in step.
#
# Deliberate differences from the PowerShell version:
#   - result lines end in LF, not CRLF.  Nothing parses these across platforms.
#   - options are --long-form rather than -Param, per shell idiom.
#   - -OutFile is still the primary output path even though a shell script has
#     no console-encoding problem, so both callers look the same.
#
# Usage:
#   gr_manifest.sh --action query|register|unregister --manifest-path PATH
#                  [--out-file PATH] [--api-version V]
#                  [--install-root PATH] [--plugin-root PATH] [--identifier PREFIX]
#                  [--components 'gr-core=4.0.0|gr-blocks=4.0.0'] [--plugins '...']
#
# --identifier records the pkgutil identifier prefix of the packages being
# registered, as 'identifier-prefix' alongside 'version'.  macOS needs it to map
# a manifest package back to the receipts gr4-uninstall has to forget; the
# PowerShell version has no use for it and never writes it, which is exactly the
# per-entry extension the schema's {"version": ...} object shape allows without
# a schema bump.  Readers must treat it as optional.
#
# Depends only on /bin/sh, plutil and awk, all of which are part of every macOS
# install.  A pkg postinstall script cannot rely on anything else being present.

set -u

schema_version=1

action=''
manifest_path=''
out_file=''
api_version=''
install_root=''
plugin_root=''
identifier=''
components=''
plugins=''

result_lines=''

add_result() {
    # $1 key, $2 value
    result_lines="${result_lines}$1=$2
"
}

write_result() {
    if [ -z "${out_file}" ]; then
        printf '%s' "${result_lines}"
        return
    fi
    printf '%s' "${result_lines}" >"${out_file}"
}

# Reports the failure the same way the PowerShell version does and stops: the
# caller reads status= from the result file, not the exit code alone.
fail() {
    add_result 'status' 'error'
    # newlines collapsed to spaces, as in the PowerShell catch block
    add_result 'message' "$(printf '%s' "$1" | tr '\n\r' '  ')"
    write_result
    exit 1
}

finish() {
    add_result 'status' 'ok'
    write_result
    exit 0
}

# --- argument parsing -------------------------------------------------------

need_value() {
    # $1 option name, $2 remaining argument count
    if [ "$2" -lt 2 ]; then
        fail "option $1 requires a value"
    fi
}

while [ $# -gt 0 ]; do
    case "$1" in
        --action) need_value "$1" $#; action="$2"; shift 2 ;;
        --manifest-path) need_value "$1" $#; manifest_path="$2"; shift 2 ;;
        --out-file) need_value "$1" $#; out_file="$2"; shift 2 ;;
        --api-version) need_value "$1" $#; api_version="$2"; shift 2 ;;
        --install-root) need_value "$1" $#; install_root="$2"; shift 2 ;;
        --plugin-root) need_value "$1" $#; plugin_root="$2"; shift 2 ;;
        --identifier) need_value "$1" $#; identifier="$2"; shift 2 ;;
        --components) need_value "$1" $#; components="$2"; shift 2 ;;
        --plugins) need_value "$1" $#; plugins="$2"; shift 2 ;;
        # the whole leading comment block, so it cannot drift out of date the
        # way a hard-coded line range does
        -h|--help) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit 0 ;;
        *) fail "unknown option '$1'" ;;
    esac
done

case "${action}" in
    query|register|unregister) ;;
    '') fail '--action is required' ;;
    *) fail "unknown action '${action}', expected query, register or unregister" ;;
esac

if [ -z "${manifest_path}" ]; then
    fail '--manifest-path is required'
fi

# plutil has no '--' end-of-options marker, so a relative path beginning with a
# dash would be read as a flag. Every caller passes an absolute path; refuse
# anything else rather than mis-parse it.
case "${manifest_path}" in
    /*) ;;
    *) fail "--manifest-path must be absolute, got '${manifest_path}'" ;;
esac

manifest_new="${manifest_path}.new"
manifest_corrupt="${manifest_path}.corrupt"

# --- helpers ----------------------------------------------------------------

json_escape() {
    # backslash first, or it would escape the quotes we just added
    printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}

manifest_is_readable() {
    # NOT plutil -lint: it only validates property lists and rejects JSON outright
    # (even '{"a":1}'), so it reported every well-formed manifest as unreadable --
    # which made 'register' move the manifest aside as corrupt and start a fresh one,
    # silently dropping every component registered by an earlier package, and made
    # 'query' always answer manifest-present=0. A conversion to a discarded output
    # parses the file without writing to it.
    plutil -convert json -o /dev/null "$1" >/dev/null 2>&1
}

# Prints one scalar, empty when the key is absent.
manifest_scalar() {
    # $1 file, $2 keypath
    plutil -extract "$2" raw -o - "$1" 2>/dev/null || true
}

# Prints the names directly under a dictionary keypath, one per line.
#
# Extracting the sub-dictionary as xml1 and reading only its top-level <key>
# elements is deliberate: plutil's compact JSON spacing is not a contract, the
# plist element structure is. The depth counter keeps each entry's own
# {"version": ...} dictionary from leaking its keys into the list. A self
# closing <dict/> (an empty section) changes no depth and yields nothing.
manifest_section_names() {
    # $1 file, $2 section
    plutil -extract "$2" xml1 -o - "$1" 2>/dev/null | awk '
        /<\/dict>/  { depth--; next }
        /<dict>/    { depth++; next }
        /<\/array>/ { depth--; next }
        /<array>/   { depth++; next }
        depth == 1 && /<key>/ {
            line = $0
            sub(/^[[:space:]]*<key>/, "", line)
            sub(/<\/key>.*$/, "", line)
            print line
        }
    '
}

write_skeleton() {
    # $1 destination
    directory=$(dirname "$1")
    mkdir -p "${directory}" || fail "cannot create ${directory}"
    cat >"$1" <<'SKELETON'
{
  "schema": 1,
  "api-version": "",
  "install-root": "",
  "plugin-root": "",
  "components": {},
  "plugins": {}
}
SKELETON
}

# Copies the manifest to the scratch file the mutating actions work on, or
# starts a fresh one. $1 is 'repair' when an unparseable manifest may be moved
# aside; reads that only report never mutate what is on disc.
begin_mutation() {
    if [ ! -f "${manifest_path}" ]; then
        write_skeleton "${manifest_new}"
        return 1
    fi
    if manifest_is_readable "${manifest_path}"; then
        cp "${manifest_path}" "${manifest_new}" || fail "cannot read ${manifest_path}"
        return 0
    fi

    add_result 'manifest-unreadable' '1'
    if [ "$1" != 'repair' ]; then
        write_skeleton "${manifest_new}"
        return 1
    fi
    mv -f "${manifest_path}" "${manifest_corrupt}" || fail "cannot move ${manifest_path} aside"
    add_result 'manifest-replaced' "${manifest_corrupt}"
    write_skeleton "${manifest_new}"
    return 1
}

# plutil writes mutations back in the input format, but says so nowhere we can
# rely on; the explicit convert guarantees JSON. -r keeps it readable, matching
# the PowerShell version's ConvertTo-Json output.
commit_mutation() {
    # a separate output path rather than converting in place: plutil would be
    # reading and writing the same file, and there is no need to find out how it
    # handles that
    plutil -convert json -r -o "${manifest_new}.json" "${manifest_new}" ||
        fail "cannot serialise ${manifest_new}"
    rm -f "${manifest_new}"
    # a crash must never leave a truncated manifest, so the real path is only
    # ever replaced by a rename of a file that is already complete
    mv -f "${manifest_new}.json" "${manifest_path}" || fail "cannot update ${manifest_path}"
}

set_scalar_if_given() {
    # $1 keypath, $2 value; blank values never clobber a recorded layout
    if [ -z "$2" ]; then
        return
    fi
    escaped=$(json_escape "$2")
    plutil -remove "$1" "${manifest_new}" >/dev/null 2>&1 || true
    plutil -insert "$1" -json "\"${escaped}\"" "${manifest_new}" ||
        fail "cannot set $1"
}

entry_name() {
    printf '%s' "${1%%=*}"
}

entry_version() {
    case "$1" in
        *=*) printf '%s' "${1#*=}" ;;
        *) printf '%s' '' ;;
    esac
}

# plutil keypaths are dot-separated, so a package name containing a dot would
# silently address a nested key instead. Refuse it rather than write the wrong
# entry; no GR package name has ever contained one.
check_name() {
    case "$1" in
        '') fail "malformed entry, expected 'name=version'" ;;
        *.*) fail "package name '$1' contains a '.', which cannot be used in a manifest key" ;;
    esac
}

# Applies $3 to every 'name=version' in the '|'-separated list $2 of section $1.
for_each_entry() {
    section="$1"
    list="$2"
    handler="$3"
    remaining="${list}"
    while [ -n "${remaining}" ]; do
        case "${remaining}" in
            *'|'*) entry="${remaining%%|*}"; remaining="${remaining#*|}" ;;
            *) entry="${remaining}"; remaining='' ;;
        esac
        [ -n "${entry}" ] || continue
        "${handler}" "${section}" "${entry}"
    done
}

# --- actions ----------------------------------------------------------------

query_section() {
    # $1 section; the reported prefix is singular, as in the PowerShell version
    prefix=$(printf '%s' "$1" | sed 's/s$//')
    manifest_section_names "${manifest_path}" "$1" | while IFS= read -r name; do
        [ -n "${name}" ] || continue
        version=$(manifest_scalar "${manifest_path}" "$1.${name}.version")
        printf '%s.%s=%s\n' "${prefix}" "${name}" "${version}"
        # optional, and absent from anything the PowerShell tool wrote
        entry_identifier=$(manifest_scalar "${manifest_path}" "$1.${name}.identifier-prefix")
        if [ -n "${entry_identifier}" ]; then
            printf 'identifier.%s=%s\n' "${name}" "${entry_identifier}"
        fi
    done
}

do_query() {
    if [ ! -f "${manifest_path}" ]; then
        add_result 'manifest-present' '0'
        return
    fi
    if ! manifest_is_readable "${manifest_path}"; then
        add_result 'manifest-unreadable' '1'
        add_result 'manifest-present' '0'
        return
    fi

    add_result 'manifest-present' '1'
    for key in install-root plugin-root api-version; do
        value=$(manifest_scalar "${manifest_path}" "${key}")
        if [ -n "${value}" ]; then
            add_result "${key}" "${value}"
        fi
    done

    # the subshell in query_section cannot append to result_lines, so its output
    # is folded back in here
    section_lines=$(query_section components; query_section plugins)
    if [ -n "${section_lines}" ]; then
        result_lines="${result_lines}${section_lines}
"
    fi
}

register_entry() {
    # $1 section, $2 'name=version'
    name=$(entry_name "$2")
    check_name "${name}"
    version=$(json_escape "$(entry_version "$2")")
    entry_json="{\"version\":\"${version}\""
    if [ -n "${identifier}" ]; then
        entry_json="${entry_json},\"identifier-prefix\":\"$(json_escape "${identifier}")\""
    fi
    entry_json="${entry_json}}"
    plutil -remove "$1.${name}" "${manifest_new}" >/dev/null 2>&1 || true
    plutil -insert "$1.${name}" -json "${entry_json}" "${manifest_new}" ||
        fail "cannot register ${name}"
    add_result 'registered' "${name}"
}

# A manifest written by an older tool, or hand-edited, may be missing a section
# entirely; -insert into a keypath whose parent does not exist fails.
ensure_section() {
    if ! plutil -extract "$1" xml1 -o - "${manifest_new}" >/dev/null 2>&1; then
        plutil -insert "$1" -json '{}' "${manifest_new}" || fail "cannot create the $1 section"
    fi
}

do_register() {
    begin_mutation repair || true

    ensure_section components
    ensure_section plugins

    plutil -remove schema "${manifest_new}" >/dev/null 2>&1 || true
    plutil -insert schema -integer "${schema_version}" "${manifest_new}" ||
        fail 'cannot set the schema version'

    set_scalar_if_given 'api-version' "${api_version}"
    set_scalar_if_given 'install-root' "${install_root}"
    set_scalar_if_given 'plugin-root' "${plugin_root}"

    for_each_entry components "${components}" register_entry
    for_each_entry plugins "${plugins}" register_entry

    commit_mutation
}

unregister_entry() {
    # $1 section, $2 'name' or 'name=version'; the version is ignored
    name=$(entry_name "$2")
    check_name "${name}"
    if plutil -remove "$1.${name}" "${manifest_new}" >/dev/null 2>&1; then
        add_result 'unregistered' "${name}"
    fi
}

section_is_empty() {
    [ -z "$(manifest_section_names "${manifest_new}" "$1")" ]
}

do_unregister() {
    if [ ! -f "${manifest_path}" ]; then
        return
    fi
    if ! begin_mutation report; then
        # nothing readable to unregister from
        rm -f "${manifest_new}"
        return
    fi

    for_each_entry components "${components}" unregister_entry
    for_each_entry plugins "${plugins}" unregister_entry

    # Plugins may deliberately outlive the runtime, so the recorded layout stays
    # until the last entry of either section is gone; a later reinstall then
    # lands in the same place without asking again. Removing the file is the
    # signal gr4-uninstall waits for before pruning the shared tree.
    if section_is_empty components && section_is_empty plugins; then
        rm -f "${manifest_new}"
        rm -f "${manifest_path}"
        add_result 'manifest-removed' '1'
        return
    fi

    commit_mutation
}

case "${action}" in
    query) do_query ;;
    register) do_register ;;
    unregister) do_unregister ;;
esac

finish
