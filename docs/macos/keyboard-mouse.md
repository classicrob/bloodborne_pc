# Keyboard and mouse

The layout follows Elden Ring's PC defaults, so Souls players can use what they already know.
Keyboard and mouse work alongside a connected gamepad. Code: `src/runtime_pad.c`
(mapping), `gpu/shim/window.cpp` (mouse capture).

**Mouse capture:** click in the game window to capture the mouse. The title bar then says
"mouse captured (Cmd releases)". Mouse motion turns the camera, and clicks attack. Press Cmd,
switch apps, open the port's settings menu or the text dialog to release the mouse. The click
that captures the mouse doesn't count as an attack.

| Action (Bloodborne) | Keyboard / mouse | PS4 | Elden Ring PC default |
|---|---|---|---|
| Move | W A S D | left stick | same |
| Walk | hold Alt (Option) + move | small stick tilt | same |
| Camera | mouse, or I J K L | right stick | mouse |
| Dodge / backstep, hold to sprint | Space | Circle | same (dash) |
| Attack | left click | R1 | same |
| Strong / charge attack | Shift + left click | R2 | same |
| Firearm / left hand (parry) | right click | L2 | guard (left hand) |
| Transform weapon | Shift + right click | L1 | skill |
| Interact / confirm | E (or Enter) | Cross | event action |
| Blood vial (heal) | R | Triangle | use item |
| Use quick item | F | Square | jump (no dedicated button in Bloodborne) |
| Lock on / reset camera | Q or middle click | R3 | same |
| L3 | X | L3 | crouch (L3 on pad) |
| D-pad: switch quick item | Down arrow | down | switch item |
| D-pad: switch left / right weapon | Left / Right arrow | left / right | same |
| D-pad up | Up arrow | up | switch spell |
| Menu (open / close) | Esc | Options | same |
| Back in menus | Space | Circle | Q |
| Gestures / left touchpad | Tab | touchpad left | |
| Right touchpad | Backspace | touchpad right | |

Free camera debug chord: hold E and press X. Port settings menu: Insert (unchanged).

**Deliberate differences from Elden Ring:**
- R drinks a Blood Vial, and F uses the selected quick item. Elden Ring has one use-item key
  because its flask is a regular item; Bloodborne has separate heal and item buttons.
  Bloodborne has no dedicated jump button (its jumps come from attacks and sprinting), so F
  is free.
- Right click fires the gun (Bloodborne's parry) where Elden Ring guards. Shift + right click
  transforms the weapon, in the place of Elden Ring's skill.
- Menus go back with Space (Circle), not Q, because the port can't tell menus from gameplay
  and Q is lock-on.
- The mouse drives a stick, so very fast flicks turn at the game's top camera speed. Set
  `BB_MOUSE_SENSITIVITY` (default `1`; above 0, at most 100) to change the speed; other values stop
  the game. The game's camera speed and axis-invert options apply as with a gamepad.
- Shift is read when a mouse button goes down. Holding a strong attack keeps it strong after
  you let go of Shift.

**Release safety:** a released mouse sends nothing. SDL releases all keys when the window
loses focus. Buttons still held when the settings menu or text dialog closes, including
L2/R2 analog values, are hidden until you release them. Scripted input (`BB_PAD_FILE`,
`BB_PAD_REPLAY`) is added to keyboard and mouse input as before. Background scripted windows
can't take focus, so the mouse is never captured during automated runs.

Sources for the Elden Ring defaults: the [Fextralife controls
page](https://eldenring.wiki.fextralife.com/Controls) and [Shacknews' PC
keybindings](https://www.shacknews.com/article/128968/elden-ring-controls-and-pc-keybindings).
Bloodborne actions: [Fextralife](https://bloodborne.wiki.fextralife.com/Controls).
