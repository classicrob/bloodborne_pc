# Central Yharnam combat fixture

`tools/macos/save_combat_fixture.py` copies the immutable
`central-yharnam-weapons-edited` fixture and changes nine bounded fields in
its active character. It is an edited test setup, not played progression.
No game acceptance is claimed by the offline checks.

The required character SHA256 is
`6d873abf6f83ce1c757a7bcc97653a3a23d899c12094d0304712a791736e2d56`,
size 1,310,720. The source has the Cleaver and Pistol equipped, 19 bullets,
and the previously qualified grounded Central Yharnam position. Its existing
verification records retain their original subjects when inherited.

| Field | Offset | Before | After |
| --- | --- | --- | --- |
| Three saved health values | `0x9df4`, `0x9df8`, `0x9dfc` | 594 each | 1100 each |
| Vitality | `0x9e20` | 11 | 30 |
| Strength | `0x9e38` | 12 | 23 |
| Level | `0x9e70` | 10 | 40 |
| Inventory count | `0xa04c` | 13 | 14 |
| Total inventory counter | `0x12458` | 78 | 79 |
| Vacant inventory entry, first 12 bytes | `0xa120` | `0, 0xffffffff, 0` | `0xb00003e8, 0x400003e8, 20` |

All values are little-endian u32s. The 19 vitality and 11 strength points
account for exactly 30 added levels. Endurance, skill, bloodtinge, arcane,
stamina, echoes, insight, equipment, ammunition, map, position, events and
the inventory entry's fourth word stay unchanged. No record is inserted or
relocated, and file size and unused suffix remain identical.

## Evidence and limits

The locally pinned Noxde editor commit
`b8eecb5d2a73066954cdb54b65349ea65b1ded62` defines the stat offsets relative
to its username anchor in
[offsets.json](https://github.com/Noxde/Bloodborne-save-editor/blob/b8eecb5d2a73066954cdb54b65349ea65b1ded62/src-tauri/resources/offsets.json).
For this exact character the anchor is `0x9e87`; the existing health triple
is 594/594/594 and the stats are 11/10/12/10/9/8, level 10.
The same source's `items.json` identifies Blood Vial as item 1000, and
`inventory.rs::add_item` describes the item tags and two counter updates.
The existing independent equipment/inventory parser checks the serialized
layout and reads the resulting vial entry back. No upstream editor is run.

The [Vitality table](https://www.bloodborne-wiki.com/2015/05/vitality.html)
lists 1100 HP at vitality 30. This is a researched preset, not a local
game-parameter extraction. Setting all three stored health values equal
requests full health; Playtest must confirm the loaded status/HP display,
healing and persistence rather than treating the offline values as proof.

## Creation and verification

```sh
python3 -B tools/macos/save_fixture_check.py
python3 -B tools/macos/save_combat_fixture.py \
  ~/bbport-game/saves/central-yharnam-weapons-edited \
  ~/bbport-game/saves/central-yharnam-combat-edited --dry-run
python3 -B tools/macos/save_combat_fixture.py \
  ~/bbport-game/saves/central-yharnam-weapons-edited \
  ~/bbport-game/saves/central-yharnam-combat-edited
```

The source hash, size, metadata name/edited status, parser layout, map,
original stat values and vacant slot are mandatory. The destination must be
new and non-nested; symlinks/special files inside the source are refused.
All source files are hashed before and after copying. Only the active
character and new fixture metadata differ; backups and global savedata are
preserved. Each change records its offset and before/after bytes; reversing
the changes must reproduce the complete source character. The dry-run prints
every changed field and creates nothing.

`fixture.json` retains the full parent and source metadata, source inventory
and hashes, edited origin, historical `verified` and `unverified_history`
arrays unchanged. Existing open items are kept; combat-fixture acceptance is
appended to `unverified`, with `validation=pending`. No new gameplay
verification record is manufactured by the tool.

Actual manual offline checks: 24 tests, exit 0, including seven combat-tool
controls for bounded changes, reversal, inventory index preservation,
ancestry/history, hash/layout refusals, source protection and failed-copy
cleanup. Real-source dry-run exits 0. These files affect no production runtime
or build target; this manual check is separate from `build.sh --test`.

Playtest acceptance: load and inspect level 40, vitality 30, strength 23,
full HP and 20 vials; retain equipped weapons/19 bullets; take damage and
heal once (vials 20 to 19); attack; normal save/quit; fresh-process reload
and inspect the persisted values. Record each observation against this
fixture's character hash, run and binary hashes with `verify-fixture`.
The starting lamp's activation and quest progression are not changed or
qualified by this edit.
