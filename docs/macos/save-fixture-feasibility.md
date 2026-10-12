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
