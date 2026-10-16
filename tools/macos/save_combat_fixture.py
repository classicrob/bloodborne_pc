#!/usr/bin/env python3
"""Copy the audited Central Yharnam fixture with level 40, full HP and 20 vials."""
import argparse
import datetime
import json
from pathlib import Path
import shutil
import struct
import sys

from save_fixture import CHARACTER, SOURCE_SIZE, canonical, digest, inventory, reverse
from save_weapon_fixture import parse, u32

SOURCE_NAME = 'central-yharnam-weapons-edited'
SOURCE_SHA256 = '6d873abf6f83ce1c757a7bcc97653a3a23d899c12094d0304712a791736e2d56'
VIAL_SLOT = 0xa120
FIELDS = (
    ('health_0', 0x9df4, 594, 1100),
    ('health_1', 0x9df8, 594, 1100),
    ('health_2', 0x9dfc, 594, 1100),
    ('vitality', 0x9e20, 11, 30),
    ('strength', 0x9e38, 12, 23),
    ('level', 0x9e70, 10, 40),
    ('inventory_count', 0xa04c, 13, 14),
    ('inventory_total_count', 0x12458, 78, 79),
)
PENDING = ['combat fixture load and level/stats/full-HP display',
           '20 blood vials and healing/consumption',
           'combat fixture attack and normal save/fresh-process reload']


def edit_character(original):
    if len(original) != SOURCE_SIZE or digest(original) != SOURCE_SHA256:
        raise ValueError('source character SHA256/size does not match audited Central fixture')
    layout = parse(original)
    if (layout['equipment_end'], layout['counter'], layout['counter2'], layout['active_end']) != (
            0x9df0, 0xa04c, 0x12458, 0x6082c):
        raise ValueError('audited character layout differs')
    if u32(original, 4) != 0x18010000:
        raise ValueError('audited Central Yharnam map differs')
    if original[VIAL_SLOT:VIAL_SLOT + 12] != struct.pack('<3I', 0, 0xffffffff, 0):
        raise ValueError('audited vial inventory slot is not vacant')
    if any(u32(b, 4) == 0x400003e8 for _, b in layout['entries']):
        raise ValueError('source already contains blood vials')
    edits = []
    data = bytearray(original)
    for name, offset, before, after in FIELDS:
        if u32(original, offset) != before:
            raise ValueError(f'audited {name} value differs')
        edits.append((name, offset, struct.pack('<I', after)))
    edits.append(('blood_vials_20', VIAL_SLOT, struct.pack('<3I', 0xb00003e8, 0x400003e8, 20)))
    changes = []
    for name, offset, value in edits:
        changes.append({'file': CHARACTER.as_posix(), 'field': name, 'offset': offset,
                        'offset_hex': hex(offset), 'before_hex': original[offset:offset+len(value)].hex(),
                        'after_hex': value.hex()})
        data[offset:offset + len(value)] = value
    edited = bytes(data)
    verify(original, edited, changes)
    return edited, changes


def verify(original, edited, changes):
    if len(edited) != len(original) or reverse(edited, changes) != original:
        raise ValueError('full-file reversal failed')
    before, after = parse(original), parse(edited)
    if before['records'] != after['records'] or before['active_end'] != after['active_end']:
        raise ValueError('equipment or serialized extent changed')
    for (offset, old), (new_offset, new) in zip(before['entries'], after['entries']):
        if new_offset != offset or (offset != VIAL_SLOT and old != new):
            raise ValueError('unrelated inventory changed')
    if dict(after['entries'])[VIAL_SLOT] != struct.pack('<3I', 0xb00003e8, 0x400003e8, 20):
        raise ValueError('vial inventory roundtrip failed')
    for name, offset, _, expected in FIELDS:
        if u32(edited, offset) != expected:
            raise ValueError(f'{name} roundtrip failed')


def create(source, destination, dry_run=False):
    source = source.expanduser().resolve(strict=True)
    destination = destination.expanduser().resolve()
    if source == destination or source in destination.parents or destination in source.parents:
        raise ValueError('source and destination must be separate, non-nested directories')
    if destination.exists() or not destination.parent.is_dir():
        raise ValueError('destination must be new, with an existing parent')
    files = inventory(source)
    parent = json.loads((source / 'fixture.json').read_text())
    if parent.get('name') != SOURCE_NAME or parent.get('edited') is not True:
        raise ValueError('expected Central Yharnam edited ancestry')
    original = (source / CHARACTER).read_bytes()
    edited, changes = edit_character(original)
    provenance = {
        'schema': 1, 'operation': 'central-yharnam-combat-level40-vials20',
        'tool': 'tools/macos/save_combat_fixture.py', 'tool_sha256': digest(Path(__file__).read_bytes()),
        'source_path': str(source), 'source_files': files,
        'source_fixture_sha256': digest(canonical(files)),
        'source_hash_definition': 'SHA256 of sorted compact ASCII JSON of source_files',
        'source_metadata': parent, 'character_before_sha256': digest(original),
        'character_after_sha256': digest(edited), 'edits': changes,
    }
    print('Source fixture SHA256:', provenance['source_fixture_sha256'])
    for c in changes:
        print(f'{c["field"]} @{c["offset_hex"]}: {c["before_hex"]} -> {c["after_hex"]}')
    if dry_run:
        print('DRY RUN: no files written')
        return provenance
    destination.mkdir()
    try:
        shutil.copytree(source, destination, dirs_exist_ok=True, symlinks=True)
        if inventory(destination) != files:
            raise ValueError('source changed during copy')
        (destination / CHARACTER).write_bytes(edited)
        metadata = {
            'name': destination.name, 'saved': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'scene': 'central-yharnam', 'edited': True, 'validation': 'pending',
            'edited_origin': parent.get('edited_origin', parent['name']), 'parent': parent,
            'unverified': list(parent.get('unverified', [])) + PENDING,
            'edit_provenance': provenance,
        }
        for key in ('verified', 'unverified_history'):
            if key in parent:
                metadata[key] = parent[key]
        (destination / 'fixture.json').write_text(json.dumps(metadata, indent=2) + '\n')
        actual = inventory(destination)
        expected = dict(files)
        expected[CHARACTER.as_posix()] = {'size': len(edited), 'sha256': digest(edited)}
        expected['fixture.json'] = actual['fixture.json']
        if actual != expected or inventory(source) != files:
            raise ValueError('source/output verification failed')
        verify(original, (destination / CHARACTER).read_bytes(), changes)
    except BaseException:
        shutil.rmtree(destination)
        raise
    print(f'Created EDITED fixture {destination}; game acceptance pending')
    return provenance


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path)
    p.add_argument('destination', type=Path)
    p.add_argument('--dry-run', action='store_true')
    args = p.parse_args()
    try:
        create(args.source, args.destination, args.dry_run)
    except (OSError, ValueError, KeyError, struct.error) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
