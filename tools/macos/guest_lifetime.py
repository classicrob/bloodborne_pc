#!/usr/bin/env python3
"""Keep a bounded entry-trace window and write it on STOP, fault, or offline EOF.

This consumes an existing game.log; it neither launches nor changes the game.
Entry ordinals are log-observation stamps, not parser submission/frame IDs.
"""
import argparse
from collections import Counter, deque
import json
from pathlib import Path
import re
import time


BASE = 0x800000000
SITES = {
    0x26A9B50: 'arena-init',
    0x26ADDC0: 'small-allocation-request',
    0x26A9DB0: 'locked-node-allocation-request',
    0x26AA5A0: 'retire-inline-entry',
    0x26AA620: 'retire-inline-secondary-entry',
    0x26AA6A0: 'retire-optional-external-entry',
    0x26AA780: 'retire-optional-external-secondary-entry',
    0x26AA860: 'reclaim-scan-entry',
    0x26AA2E0: 'immediate-free-entry',
    0x26AA440: 'immediate-free-secondary-entry',
    0x10778F0: 'eop-builder-entry',
    0x26B0B00: 'submit-wrapper-entry',
    0x26B0E30: 'frame-end-wrapper-entry',
}
FIELDS = re.compile(r'\b([a-z][a-z0-9-]*)=0x([0-9a-fA-F]+)\b')


def words(line, register):
    match = re.search(rf'\b{register}-words\[([^]]*)\]', line)
    if not match or 'unreadable' in match[1]:
        return None
    try:
        values = [int(word, 16) for word in match[1].split(',') if word]
        return values if len(values) == 8 else None
    except ValueError:
        return None


class Window:
    def __init__(self, capacity, base):
        self.records = deque(maxlen=capacity)
        self.base = base
        self.counts = Counter()
        self.stamps = Counter()
        self.total = 0

    def add(self, line, number):
        if 'GUEST-ENTRY seq=' not in line:
            return
        fields = {key: int(value, 16) for key, value in FIELDS.findall(line)}
        site = fields.get('site', 0) - self.base
        if site not in SITES:
            return
        self.total += 1
        self.counts[SITES[site]] += 1
        if site == 0x26B0B00:
            self.stamps['submit_wrapper_entries_seen'] += 1
        if site == 0x26B0E30:
            self.stamps['frame_end_wrapper_entries_seen'] += 1
        record = {
            'line': number, 'kind': SITES[site], 'site_offset': hex(site),
            'seq': fields.get('seq'), 'mach_ticks': fields.get('ticks'),
            'pthread': hex(fields.get('pthread', 0)),
            'caller': hex(fields.get('caller', 0)),
            'observed_stamps': dict(self.stamps), 'raw': line.rstrip('\n'),
        }
        node_register = ('rdx' if site in (0x26AA5A0, 0x26AA620, 0x26AA6A0, 0x26AA780)
                         else 'rsi' if site in (0x26AA2E0, 0x26AA440) else None)
        if node_register:
            record['node'] = hex(fields.get(node_register, 0))
            data = words(line, node_register)
            record['node_window_readable'] = data is not None
            if data:
                record.update(node_word0=hex(data[0]), payload_base=hex(data[1]),
                              previous_physical_node=hex(data[2]),
                              next_physical_node=hex(data[3]))
        if site == 0x26ADDC0:
            data = words(line, 'rdi')
            record.update(bucket=hex(fields.get('rdi', 0)),
                          requested_bytes=fields.get('rdx'))
            record['allocator'] = hex(data[0]) if data else None
        elif site == 0x26A9DB0:
            record.update(allocator=hex(fields.get('rdi', 0)),
                          requested_bytes=fields.get('rsi'), alignment=fields.get('rdx'))
        elif site == 0x26A9B50:
            record.update(allocator=hex(fields.get('rdi', 0)),
                          payload_base=hex(fields.get('rdx', 0)),
                          payload_size=fields.get('rcx'))
        elif site == 0x10778F0:
            record.update(label_address=hex(fields.get('rcx', 0)),
                          data_sel=fields.get('r8'), label_value=hex(fields.get('r9', 0)))
        elif site in (0x26AA6A0, 0x26AA780):
            record['external_label_argument'] = hex(fields.get('rcx', 0))
        self.records.append(record)

    def dump(self, output, reason, stop_line):
        header = {
            'type': 'window', 'reason': reason, 'stop_line': stop_line,
            'total_entries_seen': self.total, 'retained': len(self.records),
            'evicted': self.total - len(self.records), 'counts': dict(self.counts),
            'limits': 'Log-arrival window. Sequence orders handler entry, not mutations. '
                      'Stamps count wrapper entries observed, not frames/submissions completed. '
                      'No return values, per-node reclaim results or GPU completion times.',
        }
        output.write(json.dumps(header) + '\n')
        for record in self.records:
            output.write(json.dumps(record) + '\n')
        output.flush()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--capacity', type=int, default=8192)
    parser.add_argument('--base', type=lambda s: int(s, 0), default=BASE)
    parser.add_argument('--follow', action='store_true', help='Wait at EOF until STOP/fault or Ctrl-C')
    args = parser.parse_args()
    if not 1 <= args.capacity <= 100000:
        parser.error('capacity must be in [1, 100000]')
    window = Window(args.capacity, args.base)
    number = 0
    with args.log.open(errors='replace') as source, args.output.open('x') as output:
        try:
            while True:
                position = source.tell()
                line = source.readline()
                if args.follow and (not line or not line.endswith('\n')):
                    source.seek(position)
                    time.sleep(0.2)
                    continue
                if not line:
                    window.dump(output, 'offline-eof', None)
                    return 0
                number += 1
                window.add(line, number)
                if 'STOP:' in line or 'Guest fault BB_FAULT_DUMP' in line:
                    window.dump(output, 'loud-stop', line.rstrip('\n'))
                    return 0
        except KeyboardInterrupt:
            window.dump(output, 'collector-interrupted', None)
            return 130


if __name__ == '__main__':
    raise SystemExit(main())
