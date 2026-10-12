# Offline scene fixtures: feasibility

## Decision

Proceed with a **copied, explicitly edited position-only fixture**, first at
Hunter's Dream. An existing decrypted-save editor implements this as a map ID
plus three floats (16 bytes). Our played `clinic-start` matches its layout.
This is a feasible experiment, **not yet an accepted in-game save**. Playtest
must load it, obtain/equip a weapon through the game if available, autosave,
quit normally, and Continue in a fresh process. No game was run for this audit.

Do not call a coordinate warp “lamps unlocked” or “clinic events completed”.
Those progression edits are not established here. A Central Yharnam coordinate
fixture is also possible, but its lamp must be activated and verified in-game.

## Format and concrete candidate

The port's `src/runtime_savedata.c` mounts ordinary host files. It does not wrap
them in a PS4 encrypted/signed save container. The preserved fixture is under
`~/bbport-game/saves/clinic-start/savedata/1/CUSA00207/SPRJ0005/`; retain that
title path rather than deriving a different one from the installed game ID.

Read-only inspection of `userdata0000`:

| Field | Observed value |
|---|---|
| Size | 1,310,720 bytes |
| SHA256 | `f8f94f876207fbcc427b0f80866ab28f3e8b052a0e509072785164c8dbf45e5a` |
| Map at `0x04`, little-endian u32 | `0x18010000` (editor's map 24,1) |
| `FACE` / `LCED` markers | `0x118a8` / `0x3ea49` |
| Position, three little-endian f32 at `0x3ea5d` | approximately `(-195.558, -40.5304, 219.825)` |
| Editor-relative stats | level 10, health 594, echoes 300, insight 0 |

The editor locates coordinates after the first
`ff ff ff ff 00 00 00 00 00 00 00 00` marker following `LCED`.
Its Hunter's Dream preset writes `00 00 00 15` at `0x04` and
`00 00 00 c1 00 00 c0 c0 00 00 90 c1` at the coordinate field: `(-8,-6,-18)`.
Its Central Yharnam preset keeps map 24,1 and writes
`(-193.4,-28.646,68.5)` (12 bytes). These are upstream presets, not locations
qualified by our port. Offsets above apply only to this hashed source; a tool
must reject a different layout/source rather than apply them blindly.

`userdata0001..0009` have identical empty-looking headers and hashes;
`userdata0010` is a separate 262,144-byte payload. `backup0000` has the same
character layout but older coordinates/playtime. Copy the entire fixture,
including backups and sibling `SPRJ0005.sce_sys/` metadata. Initially edit only
the active character file; a load that silently recovers the original backup
is a failed validation, not success. Do not overwrite both copies to hide it.

## Weapons, lamps and flags

The editor derives inventory start as `FACE - 34028` (`0x93bc` here), with
16-byte inventory entries. Weapons additionally reference 60-byte handle,
durability and gem-slot records in the variable-length section beginning at
`0x54`. Its add-weapon path updates two counters, inserts a 60-byte record in
place of an 8-byte empty record, shifts later fields by 52 bytes, and truncates
52 bytes of the tail to keep file size fixed. Thus writing an item ID alone is
not a qualified weapon insertion. Prefer receiving the weapon in-game after
the warp; a second tool phase could implement and validate the full record
operation if that route fails.

The editor's event-flag access is relative to its username anchor plus 68,545
(`0x19da8` here); its boss table provides selected byte offsets/values. It does
**not** provide a verified unlocked-lamp/clinic-event recipe in the inspected
code. Its “teleport” changes map/position only, not a documented respawn-lamp
ID. Arbitrary flag edits risk inconsistent quests, doors and first-visit events.
No callable, version-checked guest debug-warp facility is established by this
audit; finding one is a larger RE task than testing the 16-byte save edit.

## Integrity, scope and acceptance

The upstream editor saves by writing the modified byte vector directly; its
save path has no checksum/signature regeneration. Together with matching local
fields, this supports applying decrypted-save research here. It does not prove
the game's load validation or backup selection accepts our candidate. Preserve
`param.bin`, icon, global payload and source backups for the first experiment.

Estimated implementation: one small offline Python tool, roughly 150–250 lines
plus fixture checks, with a hash/layout guard, bounds checks, copy-to-new-path,
byte-diff manifest and no in-place mode. No runtime/GPU build or guest hook is
needed. Weapon/progression editing is a separate, larger scope. The immediate
experiment changes at most 16 byte positions in a copied 1.25 MiB character
payload; the rest of the save directory is retained.

The new `fixture.json` must say `edited: true`, identify the source fixture and
its file hashes, record tool revision and each offset/before/after byte string,
and start with `validation: pending`. Retain edited ancestry even after an
in-game autosave. Playtest records loaded map/pose, weapon acquisition/equip,
lamp interaction, normal quit/reload, resulting hashes and logs. Only then
label it a usable edited scene fixture; never a played-through progression save.

## Sources and audit limits

Source-only inspection of Noxde's GPL-3.0 editor, pinned at
`b8eecb5d2a73066954cdb54b65349ea65b1ded62`:
[teleport command](https://github.com/Noxde/Bloodborne-save-editor/blob/b8eecb5d2a73066954cdb54b65349ea65b1ded62/src-tauri/src/lib.rs),
[coordinate decoder](https://github.com/Noxde/Bloodborne-save-editor/blob/b8eecb5d2a73066954cdb54b65349ea65b1ded62/src-tauri/src/data_handling/position.rs),
[destination presets](https://github.com/Noxde/Bloodborne-save-editor/blob/b8eecb5d2a73066954cdb54b65349ea65b1ded62/src/pages/character/Teleport.jsx),
[inventory writer](https://github.com/Noxde/Bloodborne-save-editor/blob/b8eecb5d2a73066954cdb54b65349ea65b1ded62/src-tauri/src/data_handling/inventory.rs),
[file and flag access](https://github.com/Noxde/Bloodborne-save-editor/blob/b8eecb5d2a73066954cdb54b65349ea65b1ded62/src-tauri/src/data_handling/file.rs).
No upstream implementation was copied into production. No save was modified,
editor executed, game launched, or load-acceptance test claimed.

## Approved fixture tool

`tools/macos/save_fixture.py` now implements the copy-and-edit operation. It is
deliberately limited to the hashed clinic character above, without an override
for unknown saves. The source is read only; destination must be new and outside
the source tree. Symlinks/special files are refused. The complete source is
hashed before and after creation, the copied output is verified, and backups
and global data are preserved byte-for-byte.

```sh
python3 -B tools/macos/save_fixture.py \
  ~/bbport-game/saves/clinic-start ~/bbport-game/saves/hunters-dream-edited --dry-run
python3 -B tools/macos/save_fixture.py \
  ~/bbport-game/saves/clinic-start ~/bbport-game/saves/hunters-dream-edited
```

Dry-run prints the source fixture hash and both before/after byte strings and
creates nothing. Creation writes `edited=true`, `validation=pending`, the source
metadata, every source file's size/hash, the complete fixture hash, edits, output
character hash and tool-content hash into `fixture.json`. The fixture hash is
SHA256 of sorted compact ASCII JSON of the file inventory; its definition is
included in the manifest. Run only against a stopped immutable fixture.

Actual manual checks: `python3 -B tools/macos/save_fixture_check.py`, seven
checks, exit 0. These use synthetic bytes, with the known-source hash replaced
only inside the test process. They cover dry-run, exact changed ranges and
independent output files, ancestry metadata, original preservation, refusal of
unknown/edited/symlink sources, existing/nested destinations, and failed-copy
cleanup. The real clinic dry-run also exited 0. No build target or runtime file
is changed; no game acceptance is implied by these filesystem checks.

Playtest owns preservation of this ancestry on subsequent `save-fixture`
exports: the source fixture metadata is in the launch manifest's `saves.from`.
Every descendant must retain `edited=true` and the original edit provenance.
Until a successful load/walk/normal-save/fresh-reload is recorded, the generated
fixture remains pending. Weapon acquisition is through the in-game path;
lamps and clinic events remain unverified.

## Central Yharnam with equipped weapons

Playtest's `20261012-010633-1080p-YHARNAM-1` demonstrated headstone travel from
Dream to the 1st Floor Sickroom lamp, followed by normal save/quit. This
qualifies **that one lamp**, not every lamp or the clinic's progression flags.
The resulting `clinic-via-sickroom-lamp` retains the equipped weapons and
edited ancestry. It is the source for the next candidate fixture.

Its character SHA256 is
`166cd3804f2a966bd49a5f68ca027d4c702f41b40ba0fbc1b186b0fc4b00fab2`,
size 1,310,720. The map is already `0x18010000` (24,1). `FACE=0x12548`,
`LCED=0x3f7a1`, coordinate marker at `0x3f7a9`, XYZ at `0x3f7b5`.
The source position is approximately `(-198.9582,-50.6399,160.5806)`.
The pinned editor's Central Yharnam preset from the sources above is
`(-193.4,-28.646,68.5)`, in the same map. It is a candidate lamp-area position;
its exact placement on our installed map has not been verified by a game run.

`save_fixture.py --scene central-yharnam` accepts only that character hash,
source metadata name and edited ancestry. It changes exactly the 12-byte XYZ
field, not map, equipment, ammunition, respawn/lamp flags, events, backups or
global save data. The edit manifest includes before/after bytes and a
full-file reversal check. It nests all source metadata under `parent` and
`source_metadata`, preserves edited origin, and copies historical verification
records unchanged if present. New position/load/walk/lamp/save acceptance is
pending. The default Dream profile keeps its existing source restriction.

```sh
python3 tools/macos/save_fixture_check.py
python3 tools/macos/save_fixture.py \
  ~/bbport-game/saves/clinic-via-sickroom-lamp \
  ~/bbport-game/saves/central-yharnam-weapons-edited \
  --scene central-yharnam --dry-run
python3 tools/macos/save_fixture.py \
  ~/bbport-game/saves/clinic-via-sickroom-lamp \
  ~/bbport-game/saves/central-yharnam-weapons-edited \
  --scene central-yharnam
```

Actual offline tests: 17 checks, exit 0. Central controls cover an unchanged
prefix/suffix around XYZ, map and ancestry retention, full-file reversal,
dry-run, source/profile refusal, guarded map/marker checks and modified edit
refusal. The real dry-run prints:

```
position_xyz @0x3f7b5: 4df546c3388f4ac2a3942043 -> 666641c3022be5c100008942
```

Playtest must first check ground/geometry placement. If the character loads
in air or inside geometry, record that result rather than qualify the preset;
a different grounded preset is the fallback. Then walk, interact with the
Central Yharnam lamp if available, attack/fire, save normally and reload in a
fresh process. This position edit does not itself unlock that lamp.

### Central Yharnam gameplay acceptance

Playtest qualified the preset on PREVIEW-26 in
`20261012-011841-1080p-CYHARNAM-1` (input
`central-yharnam-weapons-edited`) and
`20261012-012459-1080p-CYHARNAM-2-reload` (input
`central-yharnam-lamp-lit`). Both manifests pin bb-probe SHA256
`f7ffed0127cb31f5a48e5b7a47078881461663495e87e0aa30286b753af3e53f`
and libbbgpu SHA256
`8ed45f2661112b65a855e05aa2f9313172928f46a99e2cc9c313c03b8ba0f50c`.

Reported observations:

- The edited spawn stood on solid cobblestone near the clinic-side iron gate
  and Central Yharnam lamp, with both weapons and 19 bullets.
- Lighting the lamp displayed the transport/respawn confirmation, spawned
  messengers, and changed its prompt to Return to the Hunter's Dream.
- Walking around the yard and up the stairs to the next street worked.
  Searching a body yielded eight Pebbles.
- Normal save/quit passed twice. Fresh-process reload resumed at the saved
  yard-fence position with weapons and 19 bullets.
- All 33 reviewed captures were reported clean; no STOP or fault. Observed
  frame rate was 9–12 FPS, not a controlled performance baseline.

Exports `~/bbport-game/saves/central-yharnam-lamp-lit` and
`~/bbport-game/saves/central-yharnam-street` retain edited ancestry. The latter
is the exploration starting point beyond the first stairs. The lamp's lit
prompt was **not rechecked after reload**, so persistent lamp activation is
still unverified; successful reload alone does not establish it. No enemy
was encountered, and enemy combat remains untested. The item was acquired in
the second run; its persistence has not been checked by a subsequent reload.
These observations supersede the preset's pending ground/load/walk status,
without broadening them into progression or combat acceptance. Playtest owns
adding narrowly scoped verification records to fixture metadata.

### First enemy combat, death and respawn

Playtest's `20261012-014841-1080p-DBOUNDS-27db`, PREVIEW-27db, loaded
`central-yharnam-arch` and passed the formerly stopping coffin-arch trigger.
The manifest pins bb-probe
`48fe30fa1cdf0c9a482b1ff873f56f151963caf4c1bb4952ba6d080fddda453d`
and libbbgpu
`5676e4c5304e9ce7d81a29d15e7924481a78694fc42fe114cf461f5db7562d36`.

Reported acceptance: Cleaver hits landed on a townsman, with blood spray and
stagger; the enemy struck back. The player died, the YOU DIED screen rendered,
and the character respawned at the lit Central Yharnam lamp with full HP and
Blood Echoes reduced from 300 to 0. Normal save/quit completed with exit 0.
The exported `central-yharnam-after-first-death` retains edited ancestry.
No STOP/fault or rendering defect was reported in the fight.

This closes the first-enemy-combat gap for that input/build. Verification
records should name `cleaver-enemy-hit`, `enemy-retaliation`, `player-death`,
`central-lamp-respawn-in-process`, and `normal-save-quit`; inherited records
keep `central-yharnam-arch` as their tested subject. A single fight does not
qualify every combat mechanic. The respawn demonstrates the lamp's in-process
respawn registration, not a direct fresh-process check of lamp-lit state.
Lamp-lit persistence across reload and Pebble persistence remain open.
