# AGENTS.md — MeshDeck development notes

Instructions for AI agents (and humans) working on this repo. Follow these
whenever you change firmware UI or MeshDeck app sources that need a rebuild.

## Layout

| Path | Role |
|------|------|
| `firmware/` | **Source of truth** for MeshDeck app code (edit here) |
| `firmware/ui/` | Screens, DeckHW, MessageStore, UITask, etc. |
| `build/meshcore/` | MeshCore checkout used for **PlatformIO builds** |
| `build/meshcore/examples/meshdeck/` | **Compile tree** — must mirror `firmware/` |
| `platformio.local.ini` | MeshDeck env + `MESHDECK_VERSION` (repo root) |
| `build/meshcore/platformio.local.ini` | Same file in the MeshCore tree (must match root) |

PlatformIO compiles from `examples/meshdeck/`, **not** from `firmware/` directly.
If you edit only `firmware/` and forget to sync, the device will not get your
changes even after a successful build.

## Mandatory steps after any firmware change

Do **all three** before telling the user they can compile/flash:

### 1. Edit under `firmware/`

Prefer `firmware/` (and `firmware/ui/`) as the only place you implement app
changes. Keep GFX strings printable ASCII (`0x20`–`0x7E`) — the T-Deck font
cannot draw UTF-8 / em dashes / fancy punctuation.

### 2. Sync into the MeshCore example tree

Copy **all** changed files from `firmware/` → `build/meshcore/examples/meshdeck/`.

**Windows (PowerShell or cmd-friendly Python):**

```text
# Full sync of app sources (recommended after multi-file work)
python -c "from pathlib import Path; import shutil; s=Path(r'firmware'); d=Path(r'build/meshcore/examples/meshdeck');
n=0
for p in s.rglob('*'):
  if not p.is_file(): continue
  rel=p.relative_to(s)
  # Skip re-copying codec2 if already present (large); still copy if missing
  if 'codec2' in rel.parts and (d/rel).exists(): continue
  t=d/rel; t.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(p,t); n+=1
print('copied', n)"
```

**Minimal UI-only sync:**

```text
# After editing only firmware/ui/*
xcopy /E /Y /I firmware\ui\* build\meshcore\examples\meshdeck\ui\
copy /Y firmware\*.cpp build\meshcore\examples\meshdeck\
copy /Y firmware\*.h   build\meshcore\examples\meshdeck\
```

**Always verify** the critical files are identical (not just “copied”):

```text
fc /b firmware\ui\ChatScreen.cpp build\meshcore\examples\meshdeck\ui\ChatScreen.cpp
```

`FC: no differences encountered` (or byte-identical in Python) is required.
Robocopy exit code `0` often means **nothing was copied** — do not trust it alone.

### 3. Bump `MESHDECK_VERSION` and sync `platformio.local.ini`

Version lives in **both**:

- `platformio.local.ini` (repo root)
- `build/meshcore/platformio.local.ini` (compile tree)

Format:

```text
-D MESHDECK_VERSION='"1.0.0_YYMMDDx"'
```

| Part | Meaning |
|------|---------|
| `1.0.0` | Marketing / major.minor.patch (change only for intentional releases) |
| `YYMMDD` | Date of the build/change set (UTC or local day is fine; be consistent) |
| `x` | Letter suffix for same-day increments: `a`, `b`, `c`, … |

**Rules:**

- **Every** firmware/UI change set that will be flashed must bump this string.
- Same calendar day → advance the letter (`…_260810a` → `…_260810b`).
- New day → set today’s `YYMMDD` and reset letter to `a`.
- Edit the root `platformio.local.ini`, then **copy** it:

```text
copy /Y platformio.local.ini build\meshcore\platformio.local.ini
```

- Confirm both files show the same version:

```text
findstr MESHDECK_VERSION platformio.local.ini build\meshcore\platformio.local.ini
```

The splash / about UI shows this string so the user can verify the flash matched
the intended build.

## Build / flash (for the user)

Builds are run from **`build/meshcore`**, e.g.:

```text
cd build\meshcore
pio run -e MeshDeck_TDeck_868
# or beta:
pio run -e MeshDeck_TDeck_868_beta
```

Do not assume PlatformIO watches `firmware/`; the example tree is what compiles.

## Checklist (copy into PR / handoff notes)

- [ ] Changes made under `firmware/` (source of truth)
- [ ] Synced to `build/meshcore/examples/meshdeck/`
- [ ] Spot-check: key files byte-identical (`fc` / Python)
- [ ] `MESHDECK_VERSION` bumped (`1.0.0_YYMMDDx`)
- [ ] Root `platformio.local.ini` copied to `build/meshcore/platformio.local.ini`
- [ ] Both inis report the same version string
- [ ] User told new version string so they can confirm on device after flash

## Common pitfalls

1. **Editing only the example tree** — next full sync from `firmware/` will wipe it. Always edit `firmware/` first (or copy back if you had to patch the example).
2. **Partial copy** — sync *all* touched files (`AllScreens.h`, `UITask.*`, `DeckHW.*`, etc.), not just one `.cpp`.
3. **Stale build** — after sync + version bump, the user still needs a rebuild/reflash; mention the new version explicitly.
4. **UTF-8 in UI strings** — causes garbage glyphs on the T-Deck; use ASCII `-` / `...` / `->` only in user-visible strings.
5. **Fullscreen overlays** — chat overlays (long-press menus, pickers) must `fillScreen` and **early-return** from `draw()`; never paint chat then a striped dim on top (leaves underlay corruption).

## Optional helper

`tools/fix_ascii_ui_strings.py` scrubs non-ASCII punctuation under `firmware/ui` string content. Run after adding UI labels if unsure.

## Codec2 / beta

Beta builds pull codec2 sources from `examples/meshdeck/codec2/`. Prefer not to re-copy the entire codec2 tree every time unless those files changed; UI and top-level MeshDeck sources must always stay in sync.
