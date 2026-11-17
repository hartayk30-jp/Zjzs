rob run external

External, read-only ESP + aimbot + triggerbot for Rob Run. Custom overlay menu, editable keyring, config persists to disk. No injection. No D3D hook. No writes to game memory.

---

## Features

- **ESP** — boxes, health numbers, snaplines, team filter, FOV circle
- **Aimbot** — hold-to-aim, smoothing, per-tick step clamp, FOV radius limit
- **Triggerbot** — hold-to-fire, head-tolerance pixel gate, click cooldown
- **Menu** — custom-drawn dark card UI, top-left, rebindable everything, no focus steal
- **Config** — all keybinds + slider values autosave to `robrun.ini` next to the exe

Read-only external. Game values flow through `ReadProcessMemory`. Input flows through `SendInput`. The game process is never modified.

---

## Files

```
rob_run_menu.cpp   — the whole thing, one file
README.md          — this
robrun.ini         — generated on first run (binds + sliders)
```

---

## Build

From a Visual Studio Developer Command Prompt:

```
cl /std:c++20 /W4 /permissive- /O2 rob_run_menu.cpp /link user32.lib gdi32.lib
```

Output: `rob_run_menu.exe` next to the `.cpp`. Pure Win32, no dependencies.

---

## Before first run — fill the offsets

Every `0x000000` in `namespace off` is a placeholder. Dump real values from Cheat Engine against the running game:

1. Attach CE, get into a match.
2. Value-scan for your local player (health or coords).
3. Pointer-scan to a module-relative address → `dwLocalPlayer`.
4. Same for `dwEntityList`, `dwViewMatrix`, `dwCameraPos`.
5. Read the entity struct in CE's dissect view → `ent_pos`, `ent_team`, `ent_health`, `ent_bone`, `ent_live`.
6. `head_bone_idx`, `bone_stride`, `ent_stride` from watching the walk in CE.
7. Paste into their `// FILL:` slots.

`find_pid(L"RobRun.exe")` and `find_module_base(..., L"GameAssembly.dll")` — replace with the actual exe name and main module (Unity = `GameAssembly.dll`, Source = `client.dll`, custom = whatever CE shows).

Offsets need re-dumping after every game patch. Everything else survives updates.

---

## Run

1. Launch the game, get into a match.
2. **Run `rob_run_menu.exe` as administrator** (right-click → Run as administrator) — `OpenProcess` on a live game typically needs elevation.
3. Dark panel appears top-left, **OLLIE // ROB RUN**.
4. If it doesn't appear: check the message box. "game not running" = exe name wrong. "OpenProcess failed" = not admin.

---

## Default keys

| feature | default | notes |
|---|---|---|
| ESP master | `F1` | |
| Boxes | `F2` | |
| Health | `F3` | |
| Team check | `F4` | |
| Snaplines | `F5` | |
| Aimbot toggle | `F6` | master switch for aim |
| Aimbot hold | `MOUSE5` | hold to aim |
| Triggerbot toggle | `F7` | master switch for trigger |
| Triggerbot hold | `MOUSE4` | hold to autoshoot |
| Menu toggle | `INSERT` | |
| Panic exit | `END` | |

Every one of these is rebindable from the menu — click a chip, press a key, saved instantly.

---

## Using the menu

- **Click a chip** → "press a key..." → press any key or mouse4/5. Bound and saved.
- **Click a checkbox** → toggles that feature. Row highlights on hover.
- **`− [value] +`** on Smoothing / FOV → tune live. Saved on each click.
- **Clicks outside the menu panel** pass through to the game — menu stays open while you play.
- Config is `robrun.ini` next to the exe. Delete it to reset.

---

## Tuning

- **Aim overshoots** → raise smoothing toward 1.00, lower max-step, shrink FOV radius.
- **Aim too slow** → lower smoothing toward 0.05, widen FOV.
- **Triggerbot too spammy** → raise `trigDelayMs` in `robrun.ini` (25 default; 60 is careful).
- **Triggerbot never fires** → `trigFovPx` too tight (6 default) — widen to 8–10.
- **ESP empty but menu shows** → offsets wrong, or module name wrong.
- **Aim pulls but view doesn't turn** → game uses raw input; `MOUSEEVENTF_MOVE` isn't reaching the camera. Escalate to internal (`SetViewAngles`-style write, needs injection).
- **Overlay flickers in fullscreen** → switch the game to borderless windowed.

---

## What this is not

Not injected. Not a driver. Not internal. No writes to game memory. No hooked functions. Read handle + `SendInput` + a second window painted on top.

Anti-cheats that scan their own memory for modified code or injected modules have nothing to find here. Handle-strippers (blocking `OpenProcess` on the game) and kernel drivers that scan for read handles are the escalation point — that's the moment the ticket becomes internal, named in one line, then built.

---

## Licence

Local bench, private use. Do what you want.
