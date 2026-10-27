#!/usr/bin/env python3
"""Prelink original components while treating native-kernel implementations as codeless bundles."""
import argparse
import contextvars
import hashlib
import json
from pathlib import Path
import sys
from collections import Counter
import re
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/NativePrelink'))
from emit_prelink import emit as emit_candidate
import native_prelink
from native_prelink import backed_offset, exports, macho, unserialize_xml


EXECUTABLE_KEYS = ('CFBundleExecutable', '_PrelinkExecutableLoadAddr',
                   '_PrelinkExecutableSourceAddr', '_PrelinkExecutableSize',
                   '_PrelinkKmodInfo', '_PrelinkExecutableRelativePath')
_SOURCE_INTERFACE_CONTEXT = contextvars.ContextVar('source_interface_bindings', default=None)
_CURRENT_UNIQUE_KERNEL_BINDING = native_prelink.unique_kernel_binding
_BASE_UNIQUE_KERNEL_BINDING = getattr(
    _CURRENT_UNIQUE_KERNEL_BINDING, '_source_interface_base', _CURRENT_UNIQUE_KERNEL_BINDING)


def resolve_kernel_binding(address, original_exports, native_by_name):
    """Resolve only explicitly configured donor addresses in this execution context."""
    context = _SOURCE_INTERFACE_CONTEXT.get()
    if context is None or address not in context['binding_specs']:
        return _BASE_UNIQUE_KERNEL_BINDING(address, original_exports, native_by_name)
    binding = context['binding_specs'][address]
    original_names = original_exports.get(address, [])
    if original_names:
        raise ValueError(f'Explicit source-interface donor address {address:#x} unexpectedly has kernelcache exports: {original_names}')
    choices = set(native_by_name.get(binding['symbol'], set()))
    expected = context['native_addresses'][address]
    if len(choices) != 1 or next(iter(choices)) != expected:
        raise ValueError(f"Native source symbol {binding['symbol']} no longer resolves uniquely")
    context['resolved_counts'][address] += 1
    return expected


# The dispatch function is installed once and delegates to NativePrelink unchanged
# outside a ContextVar scope, so concurrent candidate builds cannot steal mappings.
resolve_kernel_binding._source_interface_base = _BASE_UNIQUE_KERNEL_BINDING
native_prelink.unique_kernel_binding = resolve_kernel_binding


def resolve_element(element, references, seen=None):
    """Resolve plist IDREF nodes without serializing away their shared identity."""
    seen = set() if seen is None else seen
    while 'IDREF' in element.attrib:
        identifier = element.attrib['IDREF']
        if identifier in seen or identifier not in references:
            raise ValueError(f'Invalid or cyclic plist IDREF: {identifier}')
        seen.add(identifier)
        element = references[identifier]
    return element


def plist_references(root):
    references = {}
    for element in root.iter():
        identifier = element.attrib.get('ID')
        if identifier is not None:
            if identifier in references:
                raise ValueError(f'Duplicate plist object ID: {identifier}')
            references[identifier] = element
    return references


def dict_value(element, key, references):
    element = resolve_element(element, references)
    if element.tag != 'dict':
        raise ValueError('Expected a plist dictionary')
    children = list(element)
    if len(children) % 2:
        raise ValueError('Odd plist dictionary')
    for index in range(0, len(children), 2):
        name = resolve_element(children[index], references)
        if name.tag == 'key' and name.text == key:
            return resolve_element(children[index + 1], references)
    return None


def rewrite_source_interfaces(original_data, source_interfaces):
    """Make selected donor bundles codeless while preserving their service metadata."""
    identifiers = sorted(set(source_interfaces))
    if not identifiers:
        return original_data, {}
    original = macho(original_data)
    info = next(segment for segment in original['segments'] if segment['name'] == '__PRELINK_INFO')
    info_at = backed_offset(original['segments'], info['va'], info['filesize'], len(original_data))
    info_blob = original_data[info_at:info_at + info['filesize']]
    xml_root = ET.fromstring(info_blob.split(b'\0', 1)[0])
    metadata = unserialize_xml(info_blob)
    entries = metadata.get('_PrelinkInfoDictionary')
    if not isinstance(entries, list):
        raise ValueError('Original kernelcache lacks a prelink info dictionary')
    references = plist_references(xml_root)
    xml_entries = dict_value(xml_root, '_PrelinkInfoDictionary', references)
    if xml_entries is not None:
        xml_entries = resolve_element(xml_entries, references)
    if xml_entries is None or xml_entries.tag != 'array':
        raise ValueError('Original kernelcache XML lacks its prelink component array')
    xml_entries_by_id = {}
    for element in xml_entries:
        element = resolve_element(element, references)
        identifier = dict_value(element, 'CFBundleIdentifier', references) if element.tag == 'dict' else None
        if identifier is None or identifier.tag != 'string':
            raise ValueError('Prelink component has no direct bundle identifier string')
        if identifier.text in xml_entries_by_id:
            raise ValueError('Original kernelcache XML has duplicate bundle identifiers')
        xml_entries_by_id[identifier.text] = element
    by_id = {}
    for entry in entries:
        identifier = entry.get('CFBundleIdentifier')
        if identifier in by_id:
            raise ValueError('Original kernelcache has duplicate bundle identifiers')
        by_id[identifier] = entry
    manifest = {}
    for identifier in identifiers:
        entry = by_id.get(identifier)
        if entry is None:
            raise ValueError(f'Missing source-interface component: {identifier}')
        if entry.get('OSKernelResource') is True or not entry.get('CFBundleExecutable') or \
                not entry.get('_PrelinkExecutableLoadAddr') or not entry.get('_PrelinkExecutableSize'):
            raise ValueError(f'Source interface must identify an executable donor bundle: {identifier}')
        personalities = entry.get('IOKitPersonalities')
        if not isinstance(personalities, dict) or not personalities:
            raise ValueError(f'Source interface requires IOKit personalities: {identifier}')
        if not isinstance(entry.get('OSBundleLibraries'), dict):
            raise ValueError(f'Source interface requires an explicit dependency dictionary: {identifier}')
        element = xml_entries_by_id.get(identifier)
        if element is None:
            raise ValueError(f'Missing source-interface plist element: {identifier}')
        module_at = backed_offset(original['segments'], entry['_PrelinkExecutableLoadAddr'],
                                 32, len(original_data))
        module = macho(original_data, module_at)
        if module['kind'] != 11:
            raise ValueError(f'Source-interface donor is not an MH_KEXT_BUNDLE: {identifier}')
        donor_segments = [{'name': segment['name'], 'va': segment['va'], 'size': segment['size'],
                           'initprot': segment['initprot']}
                          for segment in module['segments']
                          if segment['size'] and segment['name'] != '__LINKEDIT']
        if not donor_segments:
            raise ValueError(f'Source-interface donor has no mapped segments: {identifier}')
        manifest[identifier] = {
            'bundle_version': entry.get('CFBundleVersion'),
            'dependencies': sorted(entry['OSBundleLibraries']),
            'personalities': sorted(personalities),
            'segments': donor_segments,
        }
        children = list(element)
        remove = set(EXECUTABLE_KEYS)
        filtered = []
        for index in range(0, len(children), 2):
            key_node = resolve_element(children[index], references)
            if key_node.tag != 'key':
                raise ValueError('Prelink component dictionary contains a non-key entry')
            if key_node.text not in remove:
                filtered.extend(children[index:index + 2])
        element[:] = filtered
    encoded = ET.tostring(xml_root, encoding='utf-8') + b'\0'
    if len(encoded) > info['filesize']:
        raise ValueError('Codeless source-interface metadata exceeds the original prelink info extent')
    rewritten = bytearray(original_data)
    rewritten[info_at:info_at + info['filesize']] = encoded + bytes(info['filesize'] - len(encoded))
    return bytes(rewritten), manifest


def normalize_source_interface_bindings(native_data, source_manifest, bindings,
                                        kernelcache_sha256, original_digest):
    """Validate narrowly pinned donor addresses and their unique native code exports."""
    if not bindings:
        return {}, {}
    if not isinstance(kernelcache_sha256, str) or \
            not re.fullmatch(r'[0-9a-f]{64}', kernelcache_sha256) or \
            original_digest != kernelcache_sha256:
        raise ValueError('Explicit source-interface bindings require the exact pinned kernelcache SHA-256')

    normalized = {}
    for binding in bindings:
        if not isinstance(binding, dict) or set(binding) != {'bundle', 'donor_va', 'symbol'}:
            raise ValueError('Source-interface binding requires exactly bundle, donor_va, and symbol')
        bundle, symbol = binding['bundle'], binding['symbol']
        try:
            donor_va = int(binding['donor_va'], 0) if isinstance(binding['donor_va'], str) else int(binding['donor_va'])
        except (TypeError, ValueError) as exc:
            raise ValueError('Source-interface donor_va must be an integer address') from exc
        if not isinstance(bundle, str) or not bundle or not isinstance(symbol, str) or not symbol or \
                not 0 < donor_va < 1 << 64 or donor_va % 4:
            raise ValueError('Invalid source-interface binding identity or address')
        if bundle not in source_manifest:
            raise ValueError(f'Source-interface binding bundle is not declared: {bundle}')
        if donor_va in normalized:
            raise ValueError(f'Duplicate source-interface donor address: {donor_va:#x}')
        matches = [(owner, segment) for owner, entry in source_manifest.items()
                   for segment in entry['segments']
                   if segment['va'] <= donor_va < segment['va'] + segment['size']]
        if len(matches) != 1 or matches[0][0] != bundle or \
                matches[0][1]['name'] != '__TEXT_EXEC' or not matches[0][1]['initprot'] & 4:
            raise ValueError(f'Source-interface donor address {donor_va:#x} is not in one declared executable segment of {bundle}')
        normalized[donor_va] = {'bundle': bundle, 'donor_va': donor_va, 'symbol': symbol}

    native_image = native_prelink.macho(native_data)
    native_by_name = {}
    for address, names in exports(native_data, native_image).items():
        for name in names:
            native_by_name.setdefault(name, set()).add(address)
    native_addresses = {}
    for donor_va, binding in normalized.items():
        choices = native_by_name.get(binding['symbol'], set())
        if len(choices) != 1:
            raise ValueError(f"Native source symbol {binding['symbol']} has {len(choices)} address candidates")
        native_va = next(iter(choices))
        executable = [segment for segment in native_image['segments']
                      if segment['initprot'] & 4 and segment['va'] <= native_va < segment['va'] + segment['size']]
        if len(executable) != 1 or native_va % 4:
            raise ValueError(f"Native source symbol {binding['symbol']} is not in one aligned executable kernel segment")
        native_addresses[donor_va] = native_va
    return normalized, native_addresses


def build_candidate(native_data, original_data, roots, slide, early_security_init=None,
                    source_interfaces=(), source_interface_bindings=(), kernelcache_sha256=None):
    original_digest = hashlib.sha256(original_data).hexdigest()
    transformed, source_manifest = rewrite_source_interfaces(original_data, source_interfaces)
    identifiers = sorted(set(source_interfaces))
    binding_specs, native_addresses = normalize_source_interface_bindings(
        native_data, source_manifest, source_interface_bindings,
        kernelcache_sha256, original_digest)
    resolved_counts = Counter()
    binding_context = {
        'binding_specs': binding_specs,
        'native_addresses': native_addresses,
        'resolved_counts': resolved_counts,
    }
    context_token = _SOURCE_INTERFACE_CONTEXT.set(binding_context)
    try:
        image, report = emit_candidate(native_data, transformed, roots, slide, early_security_init)
    finally:
        _SOURCE_INTERFACE_CONTEXT.reset(context_token)

    missing = sorted(set(binding_specs) - set(resolved_counts))
    if missing:
        raise ValueError('Configured source-interface binding was not used by the pinned pointer table: ' +
                         ', '.join(f'{address:#x}' for address in missing))
    report['source_interface_bindings'] = [
        {**binding_specs[address], 'native_va': native_addresses[address],
         'reference_count': resolved_counts[address]}
        for address in sorted(binding_specs)
    ]
    if any(identifier not in report.get('codeless_components', []) or
           identifier in report.get('components', []) for identifier in identifiers):
        raise ValueError('Source interface was not emitted only as codeless metadata')
    source_ranges = [(segment['va'], segment['va'] + segment['size'])
                     for entry in source_manifest.values() for segment in entry['segments']]
    for binding in report.get('bindings', []):
        if any(start <= binding['target'] < end for start, end in source_ranges):
            binding['kind'] = 'source_interface'
    report['pointer_counts'] = dict(Counter(binding['kind'] for binding in report.get('bindings', [])))
    report['source_interfaces'] = identifiers
    report['source_interface_transform'] = {
        'version': 1,
        'original_kernelcache_sha256': original_digest,
        'rewritten_prelink_input_sha256': hashlib.sha256(transformed).hexdigest(),
        'entries': source_manifest,
    }
    # Preserve the hash of the immutable IPSW kernelcache as the input identity.
    report['original_sha256'] = original_digest
    return image, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--prelinked', required=True, type=Path)
    parser.add_argument('--root', required=True, action='append')
    parser.add_argument('--source-interface', action='append', default=[])
    parser.add_argument('--source-interface-binding', action='append', default=[],
                        metavar='BUNDLE=DONOR_VA=SYMBOL')
    parser.add_argument('--kernelcache-sha256')
    parser.add_argument('--slide', required=True, type=lambda value: int(value, 0))
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--early-security-init', action='append', default=[], metavar='BUNDLE=ORIGINAL_VA')
    args = parser.parse_args()
    initializers = {}
    for request in args.early_security_init:
        identifier, separator, address = request.partition('=')
        if not separator or not identifier or identifier in initializers:
            parser.error('Use one BUNDLE=ORIGINAL_VA initializer per module')
        initializers[identifier] = int(address, 0)
    source_bindings = []
    for request in args.source_interface_binding:
        bundle, separator, rest = request.partition('=')
        donor_address, separator2, symbol = rest.partition('=')
        if not separator or not separator2 or not bundle or not donor_address or not symbol or '=' in symbol:
            parser.error('Use BUNDLE=DONOR_VA=SYMBOL for each explicit source-interface binding')
        try:
            donor_va = int(donor_address, 0)
        except ValueError:
            parser.error('Source-interface donor address must be an integer literal')
        source_bindings.append({'bundle': bundle, 'donor_va': donor_va, 'symbol': symbol})
    if args.output.exists() or args.output.with_suffix('.json').exists():
        raise SystemExit('Refuse to overwrite an existing candidate or report')
    image, report = build_candidate(args.kernel.read_bytes(), args.prelinked.read_bytes(), args.root,
                                    args.slide, initializers, args.source_interface,
                                    source_bindings, args.kernelcache_sha256)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Experimental candidate: {len(image)} bytes; source interfaces are codeless; runtime unqualified')


if __name__ == '__main__':
    main()
