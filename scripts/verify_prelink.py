#!/usr/bin/env python3
"""Independently verify the structure and pointer rebinding of an emitted prelink image.

This verifier deliberately does not qualify ARM64 instruction relocation, C++ ABI,
runtime startup, or device loadability. A successful result is an audit of the
candidate's Mach-O/container structure and the original kernelcache pointer table.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/NativePrelink'))
from native_prelink import backed_offset, exports, macho, unserialize_xml


def _metadata(data, image):
    matches = [segment for segment in image['segments'] if segment['name'] == '__PRELINK_INFO']
    if len(matches) != 1:
        raise ValueError('Expected exactly one __PRELINK_INFO segment')
    segment = matches[0]
    offset = backed_offset(image['segments'], segment['va'], segment['filesize'], len(data))
    result = unserialize_xml(data[offset:offset + segment['filesize']])
    if not isinstance(result.get('_PrelinkInfoDictionary'), list):
        raise ValueError('Prelink metadata has no component dictionary')
    return result


def _entries(metadata):
    result = {}
    for entry in metadata['_PrelinkInfoDictionary']:
        if not isinstance(entry, dict) or not isinstance(entry.get('CFBundleIdentifier'), str):
            raise ValueError('Malformed prelink component record')
        identifier = entry['CFBundleIdentifier']
        if identifier in result:
            raise ValueError('Duplicate prelink component identifier: ' + identifier)
        result[identifier] = entry
    return result


def _closure(entries, roots):
    selected, interfaces = set(), set()
    pending = list(roots)
    while pending:
        identifier = pending.pop()
        if identifier in selected or identifier in interfaces:
            continue
        entry = entries.get(identifier)
        if entry is None:
            raise ValueError('Missing declared dependency: ' + identifier)
        if entry.get('OSKernelResource') is True:
            interfaces.add(identifier)
            continue
        selected.add(identifier)
        dependencies = entry.get('OSBundleLibraries') or {}
        if not isinstance(dependencies, dict):
            raise ValueError('Malformed OSBundleLibraries: ' + identifier)
        pending.extend(dependencies)
    return selected, interfaces


def _module_catalog(data, image, entries):
    modules, owners = {}, []
    for identifier, entry in entries.items():
        # XNU interface resources are represented by synthetic/prelink metadata,
        # not standalone kext Mach-Os. NativePrelink excludes them from its
        # executable component catalog for the same reason.
        if entry.get('OSKernelResource') is True:
            continue
        address = entry.get('_PrelinkExecutableLoadAddr')
        size = entry.get('_PrelinkExecutableSize')
        if not address or not size:
            continue
        header_at = backed_offset(image['segments'], address, 32, len(data))
        module = macho(data, header_at)
        if module['kind'] != 11:
            raise ValueError('Expected MH_KEXT_BUNDLE: ' + identifier)
        modules[identifier] = module
        for segment in module['segments']:
            if segment['size'] and segment['name'] != '__LINKEDIT':
                owners.append((segment['va'], segment['va'] + segment['size'], identifier, segment))
    owners.sort(key=lambda item: (item[0], item[1], item[2]))
    for previous, current in zip(owners, owners[1:]):
        if previous[1] > current[0]:
            raise ValueError(f'Overlapping original component segments: {previous[2]} and {current[2]}')
    return modules, owners


def _owner(owners, address, length=1):
    found = [item for item in owners if item[0] <= address and address + length <= item[1]]
    if len(found) > 1:
        raise ValueError(f'Ambiguous component owner for {address:#x}')
    return found[0] if found else None


def _pointer_table(metadata):
    packed = metadata.get('_PrelinkLinkKASLROffsets')
    if not isinstance(packed, bytes) or len(packed) < 4:
        raise ValueError('Malformed _PrelinkLinkKASLROffsets')
    count, = struct.unpack_from('<I', packed)
    if len(packed) != 4 + count * 4:
        raise ValueError('Pointer table length does not match its count')
    offsets = list(struct.unpack_from(f'<{count}I', packed, 4))
    if len(set(offsets)) != len(offsets):
        raise ValueError('Duplicate pointer table site')
    return offsets


def _symbols(data, image):
    by_name = {}
    for address, names in exports(data, image).items():
        for name in names:
            by_name.setdefault(name, set()).add(address)
    return by_name


def _source_maps(source_interfaces, bindings, original_modules, original_owners,
                 native_symbols, native_image):
    normalized = {}
    for item in bindings:
        if not isinstance(item, dict) or set(item) - {'bundle', 'donor_va', 'symbol', 'target_kind'}:
            raise ValueError('Malformed source-interface binding')
        if not {'bundle', 'donor_va', 'symbol'} <= set(item):
            raise ValueError('Incomplete source-interface binding')
        try:
            address = int(item['donor_va'], 0) if isinstance(item['donor_va'], str) else int(item['donor_va'])
        except (TypeError, ValueError):
            raise ValueError('Invalid source-interface donor address') from None
        bundle, symbol = item['bundle'], item['symbol']
        kind = item.get('target_kind', 'code')
        if bundle not in source_interfaces or kind not in ('code', 'data_const', 'data_rw'):
            raise ValueError('Source-interface binding is outside the declared source interfaces')
        if address in normalized:
            raise ValueError('Duplicate source-interface donor address')
        owner = _owner(original_owners, address, 1)
        if owner is None or owner[2] != bundle:
            raise ValueError(f'Source-interface donor is not owned by {bundle}: {address:#x}')
        segment = owner[3]
        native_choices = native_symbols.get(symbol, set())
        if len(native_choices) != 1:
            raise ValueError(f'Native source symbol is not unique: {symbol}')
        native_address = next(iter(native_choices))
        if kind == 'code':
            if segment['name'] != '__TEXT_EXEC' or not segment['initprot'] & 4 or native_address % 4:
                raise ValueError('Source-interface code binding has incompatible executable layout')
            native_owners = [s for s in native_image['segments']
                             if s['initprot'] & 4 and s['va'] <= native_address < s['va'] + s['size']]
            if len(native_owners) != 1:
                raise ValueError('Native source symbol is outside one executable segment')
        else:
            layout = (('__DATA_CONST', '__const', 1) if kind == 'data_const'
                      else ('__DATA', '__common', 3))
            donor_owners = [(s, sec) for s in original_modules[bundle]['segments'] for sec in s['sections']
                            if s['va'] <= address < s['va'] + s['size'] and
                            sec['va'] <= address < sec['va'] + sec['size']]
            native_owners = [(s, sec) for s in native_image['segments'] for sec in s['sections']
                             if s['va'] <= native_address < s['va'] + s['size'] and
                             sec['va'] <= native_address < sec['va'] + sec['size']]
            if len(donor_owners) != 1 or len(native_owners) != 1 or any(
                    (s['name'], sec['name'], s['initprot']) != layout for s, sec in
                    (donor_owners[0], native_owners[0])):
                raise ValueError('Source-interface data binding has incompatible segment/section layout')
        normalized[address] = {'bundle': bundle, 'symbol': symbol, 'target_kind': kind,
                               'native_va': native_address}
    return normalized


def _read_u64(data, image, address):
    offset = backed_offset(image['segments'], address, 8, len(data))
    return struct.unpack_from('<Q', data, offset)[0]


def _module_layout_check(original, original_image, original_entry, original_module,
                         candidate, candidate_image, candidate_entry, candidate_module,
                         slide, bindings):
    identifier = original_entry['CFBundleIdentifier']
    load = original_entry['_PrelinkExecutableLoadAddr']
    if candidate_entry.get('_PrelinkExecutableLoadAddr') != load + slide or \
            candidate_entry.get('_PrelinkExecutableSize') != original_entry.get('_PrelinkExecutableSize') or \
            candidate_entry.get('_PrelinkKmodInfo') != original_entry.get('_PrelinkKmodInfo', 0) + slide:
        raise ValueError('Candidate component metadata does not apply the uniform slide: ' + identifier)
    if original_module['kind'] != candidate_module['kind'] or \
            len(original_module['segments']) != len(candidate_module['segments']):
        raise ValueError('Candidate component Mach-O command layout changed: ' + identifier)

    old_load_commands = original_module['commands']
    new_load_commands = candidate_module['commands']
    if [(c, n) for c, _, n in old_load_commands] != [(c, n) for c, _, n in new_load_commands]:
        raise ValueError('Candidate component load-command sequence changed: ' + identifier)

    masked_by_segment = {}
    for binding in bindings:
        if binding['module'] == identifier:
            masked_by_segment.setdefault(binding['segment'], []).append(
                (binding['site'] - binding['segment_va'], binding['site'] - binding['segment_va'] + 8))
    kmod = original_entry.get('_PrelinkKmodInfo')
    if kmod:
        address = _read_u64(original, original_image, kmod + 156)
        if address and address != load:
            raise ValueError('Original kmod_info address disagrees with component header: ' + identifier)
        if address and _read_u64(candidate, candidate_image, kmod + slide + 156) != address + slide:
            raise ValueError('Candidate kmod_info address did not receive the uniform slide: ' + identifier)

    for old_segment, new_segment in zip(original_module['segments'], candidate_module['segments']):
        if (old_segment['name'], old_segment['size'], old_segment['filesize'], old_segment['initprot'],
                old_segment['maxprot']) != (new_segment['name'], new_segment['size'], new_segment['filesize'],
                                            new_segment['initprot'], new_segment['maxprot']):
            raise ValueError('Candidate component segment layout changed: ' + identifier)
        if old_segment['va'] and new_segment['va'] != old_segment['va'] + slide:
            raise ValueError('Candidate component segment has a nonuniform slide: ' + identifier)
        if len(old_segment['sections']) != len(new_segment['sections']):
            raise ValueError('Candidate component section layout changed: ' + identifier)
        for old_section, new_section in zip(old_segment['sections'], new_segment['sections']):
            if (old_section['name'], old_section['size'], old_section['fileoff']) != (
                    new_section['name'], new_section['size'], new_section['fileoff']) or \
                    (old_section['va'] and new_section['va'] != old_section['va'] + slide):
                raise ValueError('Candidate component section layout changed: ' + identifier)
        if not old_segment['filesize']:
            continue
        old_at = backed_offset(original_image['segments'], old_segment['va'],
                               old_segment['filesize'], len(original))
        new_at = backed_offset(candidate_image['segments'], new_segment['va'],
                               new_segment['filesize'], len(candidate))
        ignored = list(masked_by_segment.get(old_segment['name'], []))
        if kmod and old_segment['va'] <= kmod + 156 and kmod + 164 <= old_segment['va'] + old_segment['filesize']:
            ignored.append((kmod + 156 - old_segment['va'], kmod + 164 - old_segment['va']))
        if old_segment['name'] == '__TEXT':
            text_at = old_at
            for command, command_at, _length in old_load_commands:
                if command == 0x19:
                    ignored.append((command_at + 24 - text_at, command_at + 32 - text_at))
            for header_segment in original_module['segments']:
                for section in header_segment['sections']:
                    if section['size']:
                        ignored.append((section['command_offset'] + 32 - text_at,
                                        section['command_offset'] + 40 - text_at))
        ignored = sorted((max(0, start), min(old_segment['filesize'], end))
                         for start, end in ignored if start < old_segment['filesize'] and end > 0)
        cursor = 0
        old_bytes = original[old_at:old_at + old_segment['filesize']]
        new_bytes = candidate[new_at:new_at + new_segment['filesize']]
        for start, end in ignored:
            if start > cursor and old_bytes[cursor:start] != new_bytes[cursor:start]:
                raise ValueError('Candidate changed unrelocated component bytes: ' + identifier)
            cursor = max(cursor, end)
        if old_bytes[cursor:] != new_bytes[cursor:]:
            raise ValueError('Candidate changed unrelocated component bytes: ' + identifier)


def verify(native_data, original_data, candidate_data, report, roots, slide,
           source_interfaces=(), source_interface_bindings=()):
    native_image = macho(native_data)
    original_image = macho(original_data)
    candidate_image = macho(candidate_data)
    if any(image['kind'] != 2 for image in (native_image, original_image, candidate_image)):
        raise ValueError('Native kernel, kernelcache, and candidate must be arm64 MH_EXECUTE images')

    native_digest = hashlib.sha256(native_data).hexdigest()
    original_digest = hashlib.sha256(original_data).hexdigest()
    candidate_digest = hashlib.sha256(candidate_data).hexdigest()
    if report.get('native_sha256') != native_digest or report.get('original_sha256') != original_digest or \
            report.get('candidate_sha256') != candidate_digest or report.get('slide') != slide:
        raise ValueError('Prelink report hashes or slide do not identify these exact input images')
    if any(report.get(field) is not False for field in
           ('instruction_relocation_complete', 'abi_qualified', 'runtime_qualified', 'loadable')):
        raise ValueError('Audit verifier refuses a report that claims unverified loadability')

    original_metadata = _metadata(original_data, original_image)
    candidate_metadata = _metadata(candidate_data, candidate_image)
    original_entries = _entries(original_metadata)
    candidate_entries = _entries(candidate_metadata)
    selected, kernel_interfaces = _closure(original_entries, roots)
    source_interfaces = set(source_interfaces)
    if not source_interfaces <= selected:
        raise ValueError('Declared source interfaces are not in the selected dependency closure')
    codeless = {identifier for identifier in selected
                if identifier in source_interfaces or
                not original_entries[identifier].get('_PrelinkExecutableLoadAddr') or
                not original_entries[identifier].get('_PrelinkExecutableSize')}
    components = selected - codeless
    if sorted(components) != report.get('components') or sorted(codeless) != report.get('codeless_components') or \
            sorted(kernel_interfaces) != report.get('kernel_interfaces'):
        raise ValueError('Reported prelink component closure differs from the pinned original metadata')
    expected_metadata_ids = components | codeless | kernel_interfaces
    if set(candidate_entries) != expected_metadata_ids:
        raise ValueError('Candidate prelink metadata contains missing or unselected components')
    for identifier in codeless:
        entry = candidate_entries[identifier]
        executable_fields = ('CFBundleExecutable', '_PrelinkExecutableLoadAddr',
                             '_PrelinkExecutableSourceAddr', '_PrelinkExecutableSize', '_PrelinkKmodInfo')
        if any(field in entry for field in executable_fields):
            raise ValueError('Codeless candidate metadata still has executable fields: ' + identifier)

    original_modules, original_owners = _module_catalog(original_data, original_image, original_entries)
    candidate_modules, _candidate_owners = _module_catalog(candidate_data, candidate_image, candidate_entries)
    native_symbols = _symbols(native_data, native_image)
    original_symbols = exports(original_data, original_image)
    source_maps = _source_maps(source_interfaces, source_interface_bindings, original_modules,
                               original_owners, native_symbols, native_image)
    active_owners = [owner for owner in original_owners if owner[2] in components]
    original_offsets = _pointer_table(original_metadata)
    candidate_offsets = _pointer_table(candidate_metadata)
    prelink_text = [segment for segment in original_image['segments'] if segment['name'] == '__PRELINK_TEXT']
    candidate_prelink_text = [segment for segment in candidate_image['segments'] if segment['name'] == '__PRELINK_TEXT']
    if len(prelink_text) != 1 or len(candidate_prelink_text) != 1 or \
            candidate_prelink_text[0]['va'] != prelink_text[0]['va'] + slide:
        raise ValueError('Candidate __PRELINK_TEXT base does not match the uniform slide')

    coalesced_names = ('__PRELINK_TEXT', '__PLK_TEXT_EXEC', '__PLK_DATA_CONST', '__PRELINK_DATA')
    for name in coalesced_names:
        source = [segment for segment in original_image['segments'] if segment['name'] == name]
        destination = [segment for segment in candidate_image['segments'] if segment['name'] == name]
        if len(source) != 1 or len(destination) != 1 or \
                destination[0]['va'] != source[0]['va'] + slide or \
                destination[0]['size'] != source[0]['size'] or \
                destination[0]['filesize'] != source[0]['size'] or \
                destination[0]['initprot'] != source[0]['initprot'] or \
                destination[0]['maxprot'] != source[0]['initprot']:
            raise ValueError('Candidate coalesced segment does not match the original geometry: ' + name)
        for native_segment in native_image['segments']:
            if native_segment['size'] and native_segment['name'] != name and \
                    destination[0]['va'] < native_segment['va'] + native_segment['size'] and \
                    native_segment['va'] < destination[0]['va'] + destination[0]['size']:
                raise ValueError('Candidate coalesced segment collides with native kernel: ' + name)

    expected_bindings = []
    expected_offsets = []
    source_counts = Counter()
    for relative in original_offsets:
        site = prelink_text[0]['va'] + relative
        source = _owner(active_owners, site, 8)
        if source is None:
            continue
        source_id = source[2]
        old_target = _read_u64(original_data, original_image, site)
        destination = _owner(active_owners, old_target)
        interface_owner = _owner(original_owners, old_target)
        if destination is not None:
            replacement, kind = old_target + slide, 'component'
        elif interface_owner is not None and interface_owner[2] in source_interfaces:
            mapping = source_maps.get(old_target)
            if mapping is None:
                raise ValueError(f'Unmapped source-interface pointer target: {old_target:#x}')
            replacement, kind = mapping['native_va'], 'source_interface'
            source_counts[old_target] += 1
        elif interface_owner is not None:
            raise ValueError(f'Selected component points at an unselected bundle: {old_target:#x}')
        else:
            names = original_symbols.get(old_target, [])
            addresses = {address for name in names for address in native_symbols.get(name, set())}
            if len(addresses) != 1:
                raise ValueError(f'Kernel pointer target has {len(addresses)} native matches: {old_target:#x}')
            replacement, kind = next(iter(addresses)), 'kernel'
        new_site = site + slide
        if not 0 <= new_site < 1 << 64 or not 0 <= replacement < 1 << 64:
            raise ValueError('Rebound pointer is outside the 64-bit address space')
        actual = _read_u64(candidate_data, candidate_image, new_site)
        if actual != replacement:
            raise ValueError(f'Candidate pointer at {new_site:#x} is {actual:#x}, expected {replacement:#x}')
        expected_bindings.append({'site': site, 'target': old_target, 'new_site': new_site,
                                  'new_target': replacement, 'kind': kind, 'module': source_id,
                                  'segment': source[3]['name'], 'segment_va': source[3]['va']})
        expected_offsets.append(relative)

    if candidate_offsets != expected_offsets:
        raise ValueError('Candidate pointer table does not exactly cover selected original pointer sites')
    reported = report.get('bindings')
    if not isinstance(reported, list) or len(reported) != len(expected_bindings):
        raise ValueError('Prelink report binding list has the wrong size')
    keys = ('site', 'target', 'new_site', 'new_target', 'kind', 'module')
    for expected, actual in zip(expected_bindings, reported):
        if not isinstance(actual, dict) or any(actual.get(key) != expected[key] for key in keys):
            raise ValueError('Prelink report binding list differs from independently read pointer table')
    if report.get('pointer_counts') != dict(Counter(item['kind'] for item in expected_bindings)):
        raise ValueError('Prelink report pointer counts differ from the independently read pointer table')

    reported_maps = report.get('source_interface_bindings', [])
    expected_maps = []
    for donor, mapping in sorted(source_maps.items()):
        count = source_counts[donor]
        if count < 1:
            raise ValueError(f'Configured source-interface mapping is unused: {donor:#x}')
        expected_maps.append({'bundle': mapping['bundle'], 'donor_va': donor, 'symbol': mapping['symbol'],
                              'target_kind': mapping['target_kind'], 'native_va': mapping['native_va'],
                              'reference_count': count})
    for actual, expected in zip(reported_maps, expected_maps):
        if not isinstance(actual, dict) or any(actual.get(key) != value for key, value in expected.items()):
            raise ValueError('Source-interface report does not match independently resolved symbols/imports')
    if len(reported_maps) != len(expected_maps):
        raise ValueError('Source-interface report has missing or extra bindings')

    for identifier in sorted(components):
        old_entry, new_entry = original_entries[identifier], candidate_entries[identifier]
        old_load = old_entry['_PrelinkExecutableLoadAddr']
        new_load = old_load + slide
        if old_entry.get('_PrelinkExecutableSourceAddr') and \
                new_entry.get('_PrelinkExecutableSourceAddr') != old_entry['_PrelinkExecutableSourceAddr'] + slide:
            raise ValueError('Candidate executable source address did not receive the uniform slide: ' + identifier)
        old_kmod = old_entry['_PrelinkKmodInfo']
        new_kmod = old_kmod + slide
        _module_layout_check(original_data, original_image, old_entry, original_modules[identifier],
                             candidate_data, candidate_image, new_entry, candidate_modules[identifier],
                             slide, expected_bindings)
        old_at = backed_offset(original_image['segments'], old_kmod, 196, len(original_data))
        new_at = backed_offset(candidate_image['segments'], new_kmod, 196, len(candidate_data))
        old_start, old_stop = struct.unpack_from('<QQ', original_data, old_at + 180)
        new_start, new_stop = struct.unpack_from('<QQ', candidate_data, new_at + 180)
        wanted_start = old_start + slide if old_start else 0
        wanted_stop = old_stop + slide if old_stop else 0
        if (new_start, new_stop) != (wanted_start, wanted_stop):
            raise ValueError('Candidate kmod entrypoints do not match the uniform slide: ' + identifier)
        entrypoint = report.get('module_entrypoints', {}).get(identifier, {})
        if entrypoint != {'kmod_info_va': new_kmod, 'start_va': wanted_start, 'stop_va': wanted_stop}:
            raise ValueError('Prelink report kmod addresses differ from the candidate bytes: ' + identifier)

    early = report.get('early_security_initializers', {})
    candidate_early = {identifier: entry['_NativePrelinkEarlySecurityInit']
                       for identifier, entry in candidate_entries.items()
                       if '_NativePrelinkEarlySecurityInit' in entry}
    if candidate_early != early:
        raise ValueError('Candidate early-security metadata differs from the prelink report')
    for identifier, address in early.items():
        if identifier not in components or original_entries[identifier].get('AppleSecurityExtension') is not True:
            raise ValueError('Early-security initializer is not a selected security extension')
        original_address = address - slide
        if original_address % 4 or not any(
                segment['name'] == '__TEXT_EXEC' and segment['initprot'] == 5 and
                not segment['maxprot'] & 2 and segment['va'] <= original_address < segment['va'] + segment['filesize']
                for segment in original_modules[identifier]['segments']):
            raise ValueError('Early-security initializer is outside pinned executable component bytes')
        candidate_address = original_address + slide
        if not any(segment['name'] == '__TEXT_EXEC' and segment['va'] <= candidate_address <
                   segment['va'] + segment['filesize'] for segment in candidate_modules[identifier]['segments']):
            raise ValueError('Slid early-security initializer is outside candidate executable bytes')

    sparse_sections = [section for segment in candidate_image['segments'] for section in segment['sections']
                       if section['name'] == '__macho_list']
    expected_headers = sorted(candidate_entries[name]['_PrelinkExecutableLoadAddr'] for name in components)
    if len(sparse_sections) != 1 or sparse_sections[0]['size'] != len(expected_headers) * 8:
        raise ValueError('Candidate sparse Mach-O index has invalid extent')
    index_at = sparse_sections[0]['fileoff']
    if index_at + sparse_sections[0]['size'] > len(candidate_data):
        raise ValueError('Candidate sparse Mach-O index exceeds the file')
    actual_headers = list(struct.unpack_from(f'<{len(expected_headers)}Q', candidate_data, index_at))
    if actual_headers != expected_headers or report.get('sparse_index', {}).get('headers') != expected_headers:
        raise ValueError('Candidate sparse Mach-O index does not match component headers')
    return {
        'schema': 1,
        'verification': 'independent-prelink-structure-and-pointer-table',
        'native_sha256': native_digest,
        'original_sha256': original_digest,
        'candidate_sha256': candidate_digest,
        'slide': slide,
        'component_closure_verified': True,
        'pointer_table_rebindings_verified': len(expected_bindings),
        'source_interface_bindings_verified': len(expected_maps),
        'component_headers_and_bytes_verified': len(components),
        'kmod_and_sparse_index_verified': True,
        'instruction_relocation_complete': False,
        'abi_qualified': False,
        'runtime_qualified': False,
        'loadable': False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', required=True, type=Path)
    parser.add_argument('--original', required=True, type=Path)
    parser.add_argument('--candidate', required=True, type=Path)
    parser.add_argument('--report', required=True, type=Path)
    parser.add_argument('--root', action='append', required=True)
    parser.add_argument('--slide', required=True, type=lambda value: int(value, 0))
    parser.add_argument('--source-interface', action='append', default=[])
    parser.add_argument('--source-interface-binding', action='append', default=[],
                        metavar='BUNDLE=DONOR_VA=SYMBOL[=TARGET_KIND]')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    bindings = []
    for value in args.source_interface_binding:
        parts = value.split('=')
        if len(parts) not in (3, 4) or any(not part for part in parts):
            parser.error('Use BUNDLE=DONOR_VA=SYMBOL[=TARGET_KIND]')
        record = {'bundle': parts[0], 'donor_va': parts[1], 'symbol': parts[2]}
        if len(parts) == 4:
            record['target_kind'] = parts[3]
        bindings.append(record)
    audit = verify(args.native.read_bytes(), args.original.read_bytes(), args.candidate.read_bytes(),
                   json.loads(args.report.read_text()), args.root, args.slide,
                   args.source_interface, bindings)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(audit, indent=2) + '\n')
    print(json.dumps(audit, indent=2))


if __name__ == '__main__':
    main()
