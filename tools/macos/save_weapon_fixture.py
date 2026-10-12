#!/usr/bin/env python3
"""Create a reversible, hash-gated Dream weapon fixture; never edit the source."""
import argparse
import datetime
import json
from pathlib import Path
import shutil
import struct
import sys

from save_fixture import CHARACTER, SOURCE_SIZE, canonical, digest, inventory

SOURCE_SHA256 = '2021166b329e76823e15fde8373051733c2da452bd614ce565864237e5674527'
START_FIELDS = (0x1c, 0x24, 0x2c, 0x34, 0x3c, 0x44, 0x4c)
LENGTH_FIELDS = (0x10, 0x18)
GROWTH = 104


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def parse(data):
    if len(data) != SOURCE_SIZE or u32(data, 0) != 0x41:
        raise ValueError('unsupported character size/version')
    records = []
    pos = 0x54
    for slot in range(4096):
        kind = data[pos + 3]
        size = {0: 8, 0x80: 60, 0x90: 60, 0xc0: 40}.get(kind)
        if size is None or pos + size > len(data):
            raise ValueError('invalid equipment record')
        records.append((pos, bytes(data[pos:pos + size])))
        pos += size
    shift = pos - 0x832c
    face = 0x10a84 + shift
    if data[face:face + 4] != b'FACE':
        raise ValueError('unsupported inventory/appearance layout')
    counter = 0x8588 + shift
    entries = [(p, bytes(data[p:p + 12]))
               for p in range(counter + 4, counter + 4 + 1984 * 16, 16)]
    section_end = u32(data, 0x4c) + u32(data, 0x50)
    if section_end + 8 > len(data) or u32(data, section_end + 4) != 4096:
        raise ValueError('unsupported final blob')
    end = section_end + 8 + 4096 + 27
    if end > len(data):
        raise ValueError('active stream exceeds capacity')
    return {'records': records, 'equipment_end': pos, 'entries': entries,
            'counter': counter, 'counter2': 0x10994 + shift, 'active_end': end}


def patch(data, records, field, offset, value):
    before = bytes(data[offset:offset + len(value)])
    records.append({'field': field, 'offset': offset,
                    'before_hex': before.hex(), 'after_hex': value.hex()})
    data[offset:offset + len(value)] = value


def edit_character(original):
    if digest(original) != SOURCE_SHA256:
        raise ValueError('source character SHA256 does not match audited dream-exited')
    parsed = parse(original)
    if parsed['active_end'] != 0x5ebd7:
        raise ValueError('audited active stream end differs')
    empties = [p for p, b in parsed['records'] if b == struct.pack('<2I', 0, 0xffffffff)]
    if empties[:2] != [0x180, 0x188]:
        raise ValueError('audited adjacent equipment vacancies differ')
    empty_entry = struct.pack('<3I', 0, 0xffffffff, 0)
    slots = [p for p, b in parsed['entries'] if b == empty_entry][:3]
    if slots != [0x862c, 0x863c, 0x864c]:
        raise ValueError('audited inventory vacancies differ')
    largest = max(u32(b, 0) & 0xffff for _, b in parsed['records'] if b[3])
    if largest != 0x4f:
        raise ValueError('audited equipment handles differ')
    data = bytearray(original)
    changes = []
    weapons = [(0x80800050, 7000000, 200), (0x80800051, 14000000, 100)]
    inserted = b''.join(struct.pack('<5I', handle, item, durability, 0, 1)
                        + struct.pack('<2I', 0x80000000, 0) * 5
                        for handle, item, durability in weapons)
    for p, (handle, item, _) in zip(slots, weapons):
        patch(data, changes, 'weapon_inventory', p, struct.pack('<3I', handle, item, 1))
    patch(data, changes, 'quicksilver_bullets_20', slots[2],
          struct.pack('<3I', 0xb0000384, 0x40000384, 20))
    for p in (parsed['counter'], parsed['counter2']):
        patch(data, changes, 'inventory_count', p, struct.pack('<I', u32(data, p) + 3))
    for p in START_FIELDS + LENGTH_FIELDS:
        patch(data, changes, 'header_offset' if p in START_FIELDS else 'header_length',
              p, struct.pack('<I', u32(data, p) + GROWTH))
    end = parsed['active_end']
    consumed = bytes(data[end:end + GROWTH])
    # Relocate only serialized bytes. The remaining fixed-capacity suffix stays put.
    data[0x180:end + GROWTH] = inserted + data[0x190:end]
    operation = {'patches_before_relocation': changes,
                 'splice_offset': 0x180, 'removed_hex': original[0x180:0x190].hex(),
                 'inserted_hex': inserted.hex(), 'growth': GROWTH,
                 'active_end_before': end, 'active_end_after': end + GROWTH,
                 'consumed_slack_hex': consumed.hex(),
                 'unchanged_suffix_sha256': digest(original[end + GROWTH:])}
    output = bytes(data)
    verify(original, output, operation)
    return output, operation


def reverse(edited, operation):
    start = operation['splice_offset']
    end = operation['active_end_before']
    inserted = bytes.fromhex(operation['inserted_hex'])
    removed = bytes.fromhex(operation['removed_hex'])
    if edited[start:start + len(inserted)] != inserted:
        raise ValueError('inserted weapon records changed')
    restored = bytearray(edited)
    restored[start:end] = removed + edited[start + len(inserted):operation['active_end_after']]
    restored[end:operation['active_end_after']] = bytes.fromhex(operation['consumed_slack_hex'])
    for change in reversed(operation['patches_before_relocation']):
        offset = change['offset']
        after = bytes.fromhex(change['after_hex'])
        if restored[offset:offset + len(after)] != after:
            raise ValueError('output no longer matches reversible edit')
        restored[offset:offset + len(after)] = bytes.fromhex(change['before_hex'])
    return bytes(restored)


def verify(original, edited, operation):
    before, after = parse(original), parse(edited)
    if reverse(edited, operation) != original:
        raise ValueError('full-file reversal failed')
    if edited[operation['active_end_after']:] != original[operation['active_end_after']:]:
        raise ValueError('unused suffix changed')
    before_records = [b for _, b in before['records']]
    after_records = [b for _, b in after['records']]
    if after_records[:5] != before_records[:5] or after_records[7:] != before_records[7:]:
        raise ValueError('existing equipment changed')
    if len(after_records[5]) != 60 or len(after_records[6]) != 60:
        raise ValueError('weapon record roundtrip failed')
    if any(a != b for i, ((_, a), (_, b)) in enumerate(zip(before['entries'], after['entries'])) if i not in (10, 11, 12)):
        raise ValueError('existing inventory changed')
    expected = {(0x80800050, 7000000, 1), (0x80800051, 14000000, 1),
                (0xb0000384, 0x40000384, 20)}
    actual = {struct.unpack('<3I', b) for _, b in after['entries']}
    if not expected <= actual or after['active_end'] != before['active_end'] + GROWTH:
        raise ValueError('inventory/header roundtrip failed')


def create(source, destination, dry_run=False):
    source = source.expanduser().resolve(strict=True)
    destination = destination.expanduser().resolve()
    if source == destination or source in destination.parents or destination in source.parents:
        raise ValueError('source and destination must be separate, non-nested directories')
    if destination.exists() or not destination.parent.is_dir():
        raise ValueError('destination must be new, with an existing parent')
    files = inventory(source)
    parent = json.loads((source / 'fixture.json').read_text())
    if parent.get('name') != 'dream-exited' or parent.get('edited') is not True:
        raise ValueError('expected dream-exited edited ancestry')
    original = (source / CHARACTER).read_bytes()
    edited, operation = edit_character(original)
    provenance = {'schema': 1, 'operation': 'dream-starting-weapons',
                  'tool': 'tools/macos/save_weapon_fixture.py',
                  'tool_sha256': digest(Path(__file__).read_bytes()),
                  'source_path': str(source), 'source_files': files,
                  'source_fixture_sha256': digest(canonical(files)),
                  'source_hash_definition': 'SHA256 of sorted compact ASCII JSON of source_files',
                  'source_metadata': parent, 'character_after_sha256': digest(edited),
                  'edit': operation}
    print(json.dumps(provenance, indent=2))
    print('BYTE DIFF (absolute offsets; active-stream relocation included):')
    # Every changed byte is shown; no opaque region is silently omitted.
    for p in range(0, len(original), 16):
        if original[p:p + 16] != edited[p:p + 16]:
            print(f'{p:#08x}: {original[p:p + 16].hex()} -> {edited[p:p + 16].hex()}')
    if dry_run:
        print('DRY RUN: no files written')
        return provenance
    destination.mkdir()
    try:
        shutil.copytree(source, destination, dirs_exist_ok=True, symlinks=True)
        if inventory(destination) != files:
            raise ValueError('source changed during copy')
        (destination / CHARACTER).write_bytes(edited)
        metadata = {'name': destination.name, 'saved': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    'scene': 'hunters-dream', 'edited': True,
                    'edited_origin': parent.get('edited_origin', parent['name']), 'parent': parent,
                    'validation': 'pending', 'unverified': ['load/equip/attack/fire', 'save/reload',
                                                          'lamp unlocks', 'clinic events', 'messenger gifts'],
                    'edit_provenance': provenance}
        (destination / 'fixture.json').write_text(json.dumps(metadata, indent=2) + '\n')
        actual = inventory(destination)
        expected = dict(files)
        expected[CHARACTER.as_posix()] = {'size': len(edited), 'sha256': digest(edited)}
        expected['fixture.json'] = actual['fixture.json']
        if actual != expected or inventory(source) != files:
            raise ValueError('source/output verification failed')
        verify(original, (destination / CHARACTER).read_bytes(), operation)
    except BaseException:
        shutil.rmtree(destination)
        raise
    print(f'Created EDITED fixture {destination}; gameplay validation pending')
    return provenance


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    try:
        create(args.source, args.destination, args.dry_run)
    except (OSError, ValueError, KeyError, struct.error) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
