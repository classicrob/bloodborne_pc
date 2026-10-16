#!/usr/bin/env python3
"""Explicit legacy reset identity migration; never modifies or copies the original capture.

Run after disk release in a heavy slot. The caller supplies reviewed identity evidence in
--identity-map. Output links unchanged payloads and writes only new reset metadata and provenance.
The assertion authenticates the caller's mapping, not the pixels or capture independence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct

FILES = ('views.bin', 'backing.bin', 'image.bin', 'liverpool.bin', 'submissions.bin',
         'resets.bin', 'images.bin', 'image_metadata.bin')
HEADER = struct.Struct('<8sIIQ')
RESET = struct.Struct('<QIIII')
OLD = struct.Struct('<QQQQ64s')
NAMED = struct.Struct('<QQQ64s')
LIMIT = 64 << 20


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for data in iter(lambda: f.read(1 << 20), b''):
            h.update(data)
    return h.hexdigest()


def bounded_name(value):
    if not isinstance(value, str) or not value or not value.isascii() or '\0' in value:
        raise ValueError('name/module must be nonempty ASCII without NUL')
    result = value.encode('ascii')
    if len(result) >= 64:
        raise ValueError('name/module exceeds 63 bytes')
    return result


def integer(value):
    result = int(value, 0) if isinstance(value, str) else value
    if type(result) is not int or not 0 <= result < (1 << 64):
        raise ValueError('identity integers must be uint64')
    return result


def mappings(plan):
    objects = []
    for entry in plan['objects']:
        old, new = entry['old'], entry['new']
        if not isinstance(entry.get('evidence'), str) or not entry['evidence'].strip():
            raise ValueError('each semantic identity assertion needs evidence')
        old_record = (integer(old['start']), integer(old['size']), integer(old['ordinal']),
                      integer(old['offset']), bounded_name(old['module']))
        start, size = integer(new['start']), integer(new['size'])
        name = bounded_name(new['name'])
        if not size or start + size >= (1 << 64) or not old_record[1] or \
                old_record[0] + old_record[1] >= (1 << 64) or start < old_record[0] or \
                start + size > old_record[0] + old_record[1]:
            raise ValueError('named range must be wholly inside the old allocation without wrapping')
        for _, prior_start, prior_size, prior_name in objects:
            if name == prior_name or (start < prior_start + prior_size and prior_start < start + size):
                raise ValueError('duplicate name or overlapping named ranges')
        objects.append((old_record, start, size, name))
    if not objects:
        raise ValueError('no explicit identity assertions')
    return objects


def transform(data, plan):
    objects = mappings(plan)
    cursor, used, previous_end = HEADER.size, set(), 0
    if len(data) < HEADER.size or len(data) > LIMIT:
        raise ValueError('reset file missing header or exceeds 64 MiB migration limit')
    magic, version, reserved, count = HEADER.unpack_from(data)
    if (magic, version, reserved) != (b'BBRPRSET', 2, 0) or count > 1000000:
        raise ValueError('expected legacy v2 reset file')
    output = bytearray(HEADER.pack(magic, 3, 0, count))
    for _ in range(count):
        if cursor + RESET.size > len(data):
            raise ValueError('truncated reset record')
        address, size, kind, opcode, host = RESET.unpack_from(data, cursor)
        cursor += RESET.size
        if not 0 < size <= (256 << 20) or host not in (0, 1) or kind not in (1, 2, 3) or \
                address < previous_end or address + size >= (1 << 64):
            raise ValueError('invalid reset span/kind/host/order')
        previous_end = address + size
        end = cursor + size
        if end > len(data):
            raise ValueError('truncated reset payload')
        output += RESET.pack(address, size, kind, opcode, host)
        output += data[cursor:end]
        cursor = end
        if host:
            if cursor + OLD.size > len(data):
                raise ValueError('truncated legacy host identity')
            legacy = OLD.unpack_from(data, cursor)
            cursor += OLD.size
            module = legacy[4]
            if b'\0' not in module:
                raise ValueError('unterminated legacy module')
            legacy = (*legacy[:4], module.split(b'\0', 1)[0])
            candidates = [(i, start, length, name) for i, (old, start, length, name) in enumerate(objects)
                          if old == legacy and address >= start and address + size <= start + length]
            if len(candidates) != 1:
                raise ValueError('host reset has no unique exact old identity and asserted named range')
            i, start, length, name = candidates[0]
            used.add(i)
            output += NAMED.pack(start, length, address - start, name)
    if cursor != len(data) or len(used) != len(objects):
        raise ValueError('trailing reset bytes or unused identity assertion')
    return bytes(output)


def migrate(source, output, plan):
    source = source.resolve(strict=True)
    parent = output.parent.resolve(strict=True)
    if not parent.is_dir():
        raise ValueError('output parent must be a directory')
    output = parent / output.name
    if source.name.endswith('.incomplete') or source == output or source in output.parents or \
            output.is_symlink() or output.exists():
        raise ValueError('source must be complete; output must be new and different')
    if shutil.disk_usage(parent).free < 40 * 1000 ** 3 + LIMIT:
        raise ValueError('migration budget: require 40 GB free plus 64 MiB metadata budget')
    manifest_path = source / 'manifest.json'
    if digest(manifest_path) != plan['source_manifest_sha256'] or \
            digest(source / 'resets.bin') != plan['source_resets_sha256']:
        raise ValueError('source manifest/reset SHA256 differs from reviewed identity map')
    manifest = json.loads(manifest_path.read_text())
    if manifest.get('manifest_version') != 2 or set(manifest['files']) != set(FILES):
        raise ValueError('expected complete v2 capture manifest with exact file set')
    if (source / 'resets.bin').stat().st_size > LIMIT:
        raise ValueError('legacy resets exceed 64 MiB')
    transformed = transform((source / 'resets.bin').read_bytes(), plan)
    hashes = {}
    for name in FILES:
        path = source / name
        if not path.is_file() or path.stat().st_size != manifest['files'][name]:
            raise ValueError(f'{name}: source differs from manifest')
        hashes[name] = digest(path)
    if hashes['resets.bin'] != plan['source_resets_sha256'] or digest(manifest_path) != plan['source_manifest_sha256']:
        raise ValueError('source metadata changed during hashing')
    capture_hash = hashlib.sha256(json.dumps(hashes, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    provenance = {'version': 1, 'qualification': 'caller-asserted semantic identity; pixels unqualified',
                  'source': str(source), 'source_capture_sha256': capture_hash,
                  'capture_hash_definition': 'SHA256 of canonical sorted JSON filename-to-SHA256 map',
                  'source_manifest_sha256': plan['source_manifest_sha256'], 'source_files_sha256': hashes,
                  'identity_assertions': plan['objects'],
                  'transformed_resets_sha256': hashlib.sha256(transformed).hexdigest()}
    manifest['files']['resets.bin'] = len(transformed)
    manifest['resets_version'] = 3
    manifest['identity_migration'] = provenance
    text = json.dumps(manifest, indent=2) + '\n'
    if len(text.encode()) >= 8192:
        raise ValueError('overlay manifest exceeds the production loader 8191-byte limit')
    output.mkdir()  # exclusive; no cleanup/delete on failure, missing manifest means incomplete
    for name in FILES:
        if name == 'resets.bin':
            with (output / name).open('xb') as f:
                f.write(transformed)
        else:
            (output / name).symlink_to(source / name)
    with (output / 'identity-map.json').open('x') as f:
        json.dump(plan, f, indent=2); f.write('\n')
    with (output / 'manifest.json').open('x') as f:  # completion marker written last
        f.write(text)
    return provenance


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--identity-map', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = migrate(args.source, args.output, json.loads(args.identity_map.read_text()))
    except (OSError, ValueError, KeyError, TypeError, struct.error) as error:
        parser.exit(2, f'STOP: replay identity migration: {error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
