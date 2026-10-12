# Hunter's Dream: gift gates and weapon-fixture feasibility

## Accepted location fixture

Playtest reports PREVIEW-24 load, walking, normal in-game exit, and a
fresh-process Continue **PASS** for the edited Dream fixture. Runs in its
`out/macos_route/` are `20261011-231044-1080p-DREAM-1` and
`20261011-231756-1080p-DREAM-2-reload`. The exported
`~/bbport-game/saves/dream-exited` retains `edited=true`,
`edited_origin=hunters-dream-edited`, and the original edit manifest under
`parent`. This is location/save acceptance, not played-through progression.
Weapons were not obtained; lamps and clinic events remain unverified.

PREVIEW-25 `20261011-233235-1080p-DREAM-3-weapons` loads this fixture cleanly
again. Playtest revisited the headstone and a nearby small gravestone, waiting
25 seconds, but did not reach the workshop stairs. No weapon was obtained.
That does not test the identified workshop-approach gift interaction or
establish that its completion flags are set.

## Gift gates: retry the identified interaction before editing flags

The vanilla event reconstruction and its named entity definitions identify:

| Gift | Completion flag/event | Messenger / displayed gift | Offer flag | Selection flags |
| --- | --- | --- | --- | --- |
| Melee weapon | 12101020 | 2100215 / 2100220 | 12105050 | 72101000, 72101001, 72101002 |
| Firearm | 12101021 | 2100216 / 2100221 | 12105051 | 72101010, 72101011 |

Each event jumps to disabling its two entities if its completion flag is
already enabled. Otherwise it waits for messenger backread, a human player,
and distance within 6 units. It starts the arrival animation, waits 30 frames,
enables the offer flag, waits another 79 frames, then awaits a selection flag.
After selection it clears the offer flag, sets completion, and hides the gift.
The firearm path also calculates a bullet grant against the current amount of
good 900 (Quicksilver Bullets), starting from 10.

There is **no clinic/death/first-visit flag check in these two event bodies**;
the Dream constructor starts both. This is a local event-source conclusion,
not a proof about every talk-script, map-loading or engine precondition. It
also does not establish either flag's value in our save. Playtest searched
briefly near the headstone and arrival path; the headstone is a different
interaction. First retry the gift messengers on the workshop approach and
allow their frame-based animation to advance at emulated speed.

If the entities remain hidden, inspect these completion flags and their talk
conditions before changing progression. The mapping from these flag IDs to
serialized save bits is **not yet established**. Do not treat a flag ID as a
file offset, or blindly clear a byte containing unrelated flags.

Sources pinned to soulstruct-vanilla
`e6de1b79370755152f4f89c4a106a2d67c9ae674`:
[gift events](https://github.com/Grimrukh/soulstruct-vanilla/blob/e6de1b79370755152f4f89c4a106a2d67c9ae674/bloodborne/events/m21_00_00_00.evs.py),
[named entities/events](https://github.com/Grimrukh/soulstruct-vanilla/blob/e6de1b79370755152f4f89c4a106a2d67c9ae674/bloodborne/enums/m21_00_entities.py).
These are upstream reconstructed scripts, not a byte-for-byte qualification
against this installation's event archive.

## Inventory alternative: understood records, unresolved tail preservation

Noxde's pinned editor from [the initial audit](save-fixture-feasibility.md)
lists Saw Cleaver ID 7000000 and Hunter Pistol ID 14000000. An added weapon
needs a unique handle and a 60-byte durability/gem-slot record, a linked
16-byte inventory entry with quantity 1, and two inventory-counter updates.
Equipping can be left to the game. Ammunition is a separate inventory item;
possession of a pistol alone does not establish a usable ranged-combat fixture.

The current `dream-exited/userdata0000` is 1,310,720 bytes, SHA256
`2021166b329e76823e15fde8373051733c2da452bd614ce565864237e5674527`.
Read-only parsing of its 4,096 equipment-section records found 4,082 empty,
nine armor and five weapon-typed records; section end `0x832c`, first empty
`0x180`. Its inventory starts at `0x8598` (`FACE=0x10a84`), not the clinic
file's offsets. The first empty inventory entry is `0x8628`. Counters are
at `0x8588` (10) and `0x10994` (75). These observations establish capacity,
not a validated edit recipe. Existing weapon-typed records include placeholder
IDs; replacing them without understanding equipped/default references is not
an exact shortcut.

Upstream insertion replaces an 8-byte empty record with 60 bytes, shifts later
fields by 52 bytes and truncates 52 bytes from the file end. Two weapons would
shift by 104 bytes. **This save's last 104 bytes are nonzero, pixel-like data**;
their role and whether truncation is legitimate have not been established.
Do not copy that truncation policy into our guarded tool or call it an exact,
metadata-preserving edit. Locating the variable payload/trailer boundary or
obtaining a game-produced weapon record pair is the remaining serialization
work. A tool would then be a few hundred lines of guarded record parsing,
relocation, provenance and tests, followed by equip/attack/fire/save/reload
acceptance on a new marked fixture. No inventory or flags were edited here.

Recommendation: the identified gift interaction is the smallest next test.
Reserve inventory insertion for a demonstrated unavailable gift; do not infer
missing clinic flags from the first short search.


## Audited inventory fixture (01.09, dream-exited only)

The inventory tool is now `tools/macos/save_weapon_fixture.py`. It accepts only
this exact source character SHA256 and the `dream-exited` edited metadata.
It adds Saw Cleaver 7000000 (durability 200), Hunter Pistol 14000000
(durability 100), and 20 Quicksilver Bullets (good 900). It does not equip them,
change gift flags, or replace the default/fist records. Load, equip, attack,
fire, and save/reload remain Playtest acceptance tasks.

The tail uncertainty above is resolved for this source by static analysis of
its 01.09 serializer `0x156aa50` and loader `0x156b970` (image offsets):

- The header is 0x54 bytes. `0x14ea560` writes the first subsection offsets
  and sizes. The main serializer writes the remaining section starts/sizes.
  Two new 60-byte equipment records replace two empty 8-byte records,
  requiring 104 bytes. Header starts +0x1c/24/2c/34/3c/44/4c and containing
  lengths +0x10/18 therefore increase by 104; all other header bytes stay.
- The last header-indexed section ends at +0x4c + +0x50 = 0x5dbb4.
  `0x175a4e0` writes an 8-byte header followed by its size field's 4096 bytes;
  `0x175a6a0` reads the same structure and checks its size.
- Version 0x41 then serializes 27 bytes: 1+4 (`0x156b549` onward),
  1+4+4 (version >=0x35), 1 (>=0x37), and 12 (>=0x36).
  The loader mirrors those reads at `0x156c36d` through `0x156c5aa`.
  The active stream therefore ends at **0x5ebd7**, well before the fixed
  0x140000-byte save-buffer capacity. No checksum or signature is added by
  this serializer; this is not a statement about encrypted console containers.

Only that active stream moves. Its new end is **0x5ec3f**. The file size stays
0x140000; bytes from 0x5ec3f to EOF remain at their original offsets and are
byte-for-byte identical. The 104 unused bytes consumed by expansion are saved
verbatim in provenance, allowing exact full-file reversal. This does not use
the reference editor's whole-file shift and EOF truncation.

The parser walks all 4096 equipment records, finds the three empty inventory
entries, validates the final blob and parses the output again. Every existing
equipment/inventory record is preserved. Two inventory counters increase by
three. Inventory entry index words are preserved, matching the reference
format's empty-slot insertion; gameplay acceptance remains necessary.
The weapon record layout is independently constructed from its documented
fields; pinned reference-editor fixtures also contain pistol durability 100
and cleaver durability 200 with five closed gem slots.

The dry-run emits every changed 16-byte row plus semantic edits, relocation,
source-file hashes and consumed slack. The new fixture retains `edited=true`,
its complete parent metadata and source hash. Source and output trees are
verified after copying. The tool refuses another source, existing/nested
output, non-regular files, and unsupported layouts. No game runs are performed
by the tool.

```sh
python3 tools/macos/save_fixture_check.py
python3 tools/macos/save_weapon_fixture.py \
  ~/bbport-game/saves/dream-exited ~/bbport-game/saves/dream-weapons-edited \
  --dry-run > out/save-fixture-audit/weapon-dry-run.txt
python3 tools/macos/save_weapon_fixture.py \
  ~/bbport-game/saves/dream-exited ~/bbport-game/saves/dream-weapons-edited \
  > out/save-fixture-audit/weapon-create.txt
```

Manual checks: 13 tests, covering the original location tool and the weapon
fixture's dry-run, full-file reversal, unchanged tail, preserved ancestry,
source-hash refusal, destination refusal, copy-failure cleanup and modified
edit rejection. These are offline format/filesystem checks, not game-load
acceptance.


## Gameplay acceptance and verification metadata

Playtest qualified `dream-weapons-edited` on PREVIEW-26. Its runs are
`20261012-005806-1080p-WEAPONS-1` and
`20261012-010415-1080p-WEAPONS-2-reload`, under the Playtest worktree's
`out/macos_route/`.

- Dream load and equipment-menu selection passed for both weapons. Reported
  durability was 200 for the Cleaver and 100 for the Pistol.
- Cleaver attack spent stamina; its trick transformation worked.
- Pistol fire consumed a bullet, 20 to 19.
- Normal in-game save/quit passed. A fresh process loaded the exported
  `dream-weapons-equipped` at the lantern with both weapons equipped and
  19 bullets. No STOP or guest fault was reported; rendering was clean in
  the captures. The known boot audio warning remained.

The pinned binary SHA256 values, read from both run manifests, are:

- bb-probe: `f7ffed0127cb31f5a48e5b7a47078881461663495e87e0aa30286b753af3e53f`
- libbbgpu: `8ed45f2661112b65a855e05aa2f9313172928f46a99e2cc9c313c03b8ba0f50c`

This is acceptance of the edited inventory fixture. It does not establish
messenger gifts, clinic progression flags, unlocked lamps, or combat against
an enemy. Earlier pending statements above describe the pre-test state.

Fixture metadata records these observations as an additive `verified` array.
Each entry contains `item`, `fixture`, `character_sha256`, `run`, `build`,
`artifacts` (binary hashes), and `evidence` (capture/action). Items are narrow:
`load`, `equip-cleaver`, `equip-pistol`, `cleaver-attack`, `cleaver-transform`,
`pistol-fire`, `normal-save-quit`, and `fresh-process-reload`. Fixture and
character hash identify the input tested in that run. Run 1 tests
`dream-weapons-edited`; run 2 directly tests `dream-weapons-equipped`.

Descendants inherit those entries verbatim, retaining the tested subject;
an inherited record is historical evidence, not a claim that the descendant
was retested. A binary build label is accompanied by artifact hashes, since
a manifest's launcher-checkout HEAD need not identify its pinned binaries.
`parent` and `edit_provenance` remain unchanged, as do `edited=true` and the
edited origin. Before narrowing top-level `unverified`, retain the previous
list in `unverified_history` entries with `items`, `reason`, and `runs`.
Use `validation=partial`: lamps, clinic events and messenger gifts remain
unverified. Playtest owns the route-tool implementation and fixture metadata
annotation; this document does not claim those updates have executed yet.
