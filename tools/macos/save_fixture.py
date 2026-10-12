#!/usr/bin/env python3
"""Copy an audited fixture and edit only its scene position/map."""

import argparse
import datetime
import hashlib
import json
from pathlib import Path
import shutil
import stat
import struct
import sys


CHARACTER = Path('savedata/1/CUSA00207/SPRJ0005/userdata0000')
SOURCE_SHA256 = 'f8f94f876207fbcc427b0f80866ab28f3e8b052a0e509072785164c8dbf45e5a'
SOURCE_SIZE = 1310720
CENTRAL_SOURCE_SHA256 = '166cd3804f2a966bd49a5f68ca027d4c702f41b40ba0fbc1b186b0fc4b00fab2'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode()


def inventory(root):
    """Hash regular files without following directory or file symlinks."""
    result = {}
    for path in sorted(root.rglob('*')):
        mode = path.lstat().st_mode
        if stat.S_ISDIR(mode):
            continue
        if not stat.S_ISREG(mode):
            raise ValueError(f'not a regular file/directory: {path}')
        data = path.read_bytes()
        result[path.relative_to(root).as_posix()] = {
            'size': len(data), 'sha256': digest(data),
        }
    return result


def plan(source, destination, scene="hunters-dream"):
    source = source.expanduser().resolve(strict=True)
    destination = destination.expanduser().resolve()
    if not source.is_dir():
        raise ValueError('source must be a stopped, saved fixture directory')
    if source == destination or source in destination.parents or destination in source.parents:
        raise ValueError('source and destination must be separate, non-nested directories')
    if destination.exists():
        raise ValueError(f'destination already exists: {destination}')
    if not destination.parent.is_dir():
        raise ValueError('destination parent must already exist')
    files = inventory(source)
    metadata = json.loads((source / 'fixture.json').read_text())
    if scene == 'hunters-dream' and isinstance(metadata, dict) and (metadata.get('edited') or metadata.get('edit_provenance')):
        raise ValueError('source must have played-fixture metadata, not edited ancestry')
    if not isinstance(metadata, dict) or not metadata.get('name') or not metadata.get('run'):
        raise ValueError('source metadata must identify its name and played run')
    if scene == 'hunters-dream':
        expected_hash = SOURCE_SHA256
        face, lced = 0x118a8, 0x3ea49
        fields = (('map', 4, struct.pack('<I', 0x15000000)),
                  ('position_xyz', 0x3ea5d, struct.pack('<3f', -8, -6, -18)))
    elif scene == 'central-yharnam':
        if metadata['name'] != 'clinic-via-sickroom-lamp' or metadata.get('edited') is not True:
            raise ValueError('Central Yharnam requires clinic-via-sickroom-lamp edited ancestry')
        expected_hash = CENTRAL_SOURCE_SHA256
        face, lced = 0x12548, 0x3f7a1
        fields = (('position_xyz', 0x3f7b5, struct.pack('<3f', -193.4, -28.646, 68.5)),)
    else:
        raise ValueError('unsupported scene')
    data = (source / CHARACTER).read_bytes()
    if len(data) != SOURCE_SIZE or digest(data) != expected_hash:
        raise ValueError('character does not match the audited source SHA256/size')
    if data[face:face + 4] != b'FACE' or data[lced:lced + 4] != b'LCED':
        raise ValueError('audited FACE/LCED layout differs')
    if scene == 'central-yharnam' and (
        data[4:8] != struct.pack('<I', 0x18010000) or
        data[lced + 8:lced + 20] != bytes.fromhex('ffffffff0000000000000000')
    ):
        raise ValueError('audited Central Yharnam map/coordinate marker differs')
    edits = []
    edited = bytearray(data)
    for field, offset, after in fields:
        before = data[offset:offset + len(after)]
        edits.append({'file': CHARACTER.as_posix(), 'field': field,
                      'offset': offset, 'offset_hex': hex(offset),
                      'before_hex': before.hex(), 'after_hex': after.hex()})
        edited[offset:offset + len(after)] = after
    provenance = {
        'schema': 1,
        'operation': scene + '-position-only',
        'tool': 'tools/macos/save_fixture.py',
        'tool_sha256': digest(Path(__file__).read_bytes()),
        'source_path': str(source),
        'source_fixture_sha256': digest(canonical(files)),
        'source_hash_definition': 'SHA256 of sorted compact ASCII JSON of source_files',
        'source_files': files,
        'source_metadata': metadata,
        'edits': edits,
        'character_after_sha256': digest(edited),
    }
    if reverse(edited, edits) != data:
        raise ValueError("full-file edit reversal failed")
    return source, destination, bytes(edited), provenance


def reverse(data, edits):
    restored = bytearray(data)
    for edit in reversed(edits):
        offset = edit['offset']
        before, after = bytes.fromhex(edit['before_hex']), bytes.fromhex(edit['after_hex'])
        if len(before) != len(after) or restored[offset:offset + len(after)] != after:
            raise ValueError('output no longer matches position edit')
        restored[offset:offset + len(before)] = before
    return bytes(restored)


def create(source, destination, dry_run=False, scene="hunters-dream"):
    source, destination, edited, provenance = plan(source, destination, scene)
    print(f'Source fixture SHA256: {provenance["source_fixture_sha256"]}')
    for edit in provenance['edits']:
        print(f'{edit["file"]} {edit["field"]} @{edit["offset_hex"]}: '
              f'{edit["before_hex"]} -> {edit["after_hex"]}')
    if dry_run:
        print(f'DRY RUN: would create {destination}; no files written')
        return provenance

    # Exclusive directory creation prevents replacing any existing fixture.
    destination.mkdir()
    try:
        shutil.copytree(source, destination, dirs_exist_ok=True, symlinks=True)
        if inventory(destination) != provenance['source_files']:
            raise ValueError('source changed during copy; copied fixture rejected')
        (destination / CHARACTER).write_bytes(edited)
        metadata = {
            'name': destination.name,
            'saved': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'edited': True,
            'validation': 'pending',
            'scene': scene,
            'parent': provenance['source_metadata'],
            'edited_origin': provenance['source_metadata'].get('edited_origin', destination.name),
            'unverified': ['game load acceptance', 'weapon acquisition',
                           'lamp unlocks', 'clinic event completion', 'save/reload'],
            'edit_provenance': provenance,
        }
        if scene == 'central-yharnam':
            metadata['unverified'] = ['Central Yharnam ground/load/walk', 'Central Yharnam lamp interaction',
                                      'Central Yharnam save/reload', 'clinic event completion', 'messenger gifts']
        if 'verified' in provenance['source_metadata']:
            metadata['verified'] = provenance['source_metadata']['verified']
        (destination / 'fixture.json').write_text(json.dumps(metadata, indent=2) + '\n')
        expected = dict(provenance['source_files'])
        expected[CHARACTER.as_posix()] = {'size': len(edited), 'sha256': digest(edited)}
        actual = inventory(destination)
        expected['fixture.json'] = actual['fixture.json']
        if actual != expected or inventory(source) != provenance['source_files']:
            raise ValueError('source or output changed during creation; fixture rejected')
    except BaseException:
        shutil.rmtree(destination)
        raise
    print(f'Created EDITED fixture: {destination} (validation pending; no game run)')
    return provenance


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='played fixture; read only, never modified')
    parser.add_argument('destination', type=Path, help='new directory; refuses overwrite/in-place edits')
    parser.add_argument('--dry-run', action='store_true', help='print hashes and before/after bytes only')
    parser.add_argument('--scene', choices=('hunters-dream', 'central-yharnam'), default='hunters-dream')
    args = parser.parse_args()
    try:
        create(args.source, args.destination, args.dry_run, args.scene)
    except (OSError, ValueError, KeyError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
