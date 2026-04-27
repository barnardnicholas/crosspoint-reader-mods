# Fork Decisions and Modifications

This is a personal fork of [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader).
It tracks upstream but adds features that are either exploratory, personal-use only, or not yet ready for upstream submission.

**Fork repo**: `barnardnicholas/crosspoint-reader-mods`  
**Upstream repo**: `crosspoint-reader/crosspoint-reader`  
**Remotes**: `origin` = fork, `upstream` = upstream

---

## Changes from Upstream

### 1. Dark Mode (complete)

**Commits**: `c85fba9` → `8455418` (9 commits)

Dark mode (`SETTINGS.darkMode`) inverts the framebuffer globally across all screens — menus, settings, and all reader formats.

**Why this fork implements it differently from a trivial invert**:

- E-ink displays using hardware grayscale LUTs produce rendering artifacts when the framebuffer is inverted. Anti-aliasing and XTC grayscale passes must be disabled when `darkMode == 1`.
- Menu screen transitions ghost badly with a straight invert. The fix: `Activity::onEnter()` sets `halfRefreshPending = true`, and `Activity::menuDisplay()` consumes it with a `HALF_REFRESH` on first render to clear the ghost.
- Readers need a `FULL_REFRESH` on exit back to menus (`ReaderUtils::fullRefreshOnExit`) to clear inverted ghost pixels before normal (non-inverted) menu rendering resumes.

**Key design decisions**:

| Decision | Rationale |
|---|---|
| Global `SETTINGS.darkMode` (not per-screen) | Simpler; no mixed-invert state that causes ghost artifacts at transitions |
| `menuDisplay()` replaces `applyDarkModeIfEnabled + displayBuffer` | Centralises half-refresh logic so individual activities don't manage it |
| `requestHalfRefresh()` API on Activity | Allows intra-activity refresh reset (e.g. settings tab switch) without re-entering the activity |
| Grayscale/AA disabled when dark mode active | Hardware LUTs assume white background; enabling both produces severe artifacts |
| `ReaderUtils::fullRefreshOnExit` | Clears inverted ghost before menus re-render; skipped on `BmpViewerActivity` and `FullScreenMessageActivity` (have their own refresh logic) |

**Pattern for all menu/UI activities**:
```cpp
// end of every render function:
menuDisplay();

// on significant content change within activity:
requestHalfRefresh();

// in reader onExit():
ReaderUtils::fullRefreshOnExit(renderer);
```

See `docs/contributing/architecture.md` § "Dark mode and display refresh" for the full reference.

---

### 2. MOBI Format Support (WIP)

**Commits**: `a483d01`, `c7a8548`

Adds basic `.mobi` (PalmDOC/PalmDB) file reading via `lib/Mobi/` and `src/activities/reader/MobiReaderActivity`.

**Current status**: functional but marked WIP. Not all MOBI subtypes supported.

**Supported**: PalmDOC-compressed (type 2) and uncompressed (type 1).  
**Not supported**: Huffman-compressed MOBI (type 17480 / KF8 / modern Kindle format).

**Architecture**: `Mobi` class presents a "virtual flat file" interface — `readContent(byteOffset, length)` decompresses on demand. `MobiReaderActivity` reuses `TxtReaderActivity` pagination logic unchanged. A virtual offset table (`voffsets.bin`) is cached to SD to give O(log n) random access without holding the full decompressed text in RAM.

**Key constraint**: Decompressing the full MOBI text would blow the 380KB RAM ceiling. The virtual offset approach decompresses only the current 8KB window.

**Cache**: `.crosspoint/mobi_<hash>/voffsets.bin` — invalidated if font, margin, or orientation settings change (same bust parameters as TXT reader).

**Why not upstream**: Still WIP — no TOC navigation, no metadata cover art in library, limited subtype support. Needs more testing before a PR to upstream is appropriate.

---

### 3. SDK Submodule Bump

**Commit**: `9ee6a5e`

Bumped `open-x4-sdk` submodule to a newer revision. Tracks SDK changes independently of upstream CrossPoint cadence.

---

## Sync Strategy with Upstream

This fork regularly merges upstream `master` into `origin/master`. When merging:

1. `git fetch upstream`
2. `git merge upstream/master`
3. Resolve conflicts in `src/CrossPointSettings.h` (settings list) and `src/SettingsList.h` (settings UI order) — dark mode adds entries that may conflict with upstream additions.
4. Regenerate i18n: `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/`

Dark mode strings are in all translation YAML files as `STR_DARK_MODE`. Any upstream translation additions must be preserved.

---

## What AI Agents Should Know

- **MOBI is WIP**: `MobiReaderActivity` and `lib/Mobi/` are fork-only. Do not assume upstream has these. Do not suggest upstreaming without noting the WIP status.
- **Dark mode is the primary completed fork feature**: All display refresh patterns in `CLAUDE.md` and `architecture.md` exist because of this feature. Do not remove or simplify `menuDisplay()`, `halfRefreshPending`, or `fullRefreshOnExit` — these solve real hardware artifacts.
- **`darkMode` setting is global**: Upstream has no `SETTINGS.darkMode`. If merging upstream `CrossPointSettings.h` changes, preserve this field.
- **`STR_DARK_MODE` i18n key is fork-only**: Present in all YAML translation files. Preserve on upstream merges.
- **Don't add new display calls without using `menuDisplay()`**: All non-reader render functions must end with `menuDisplay()`, not bare `renderer.displayBuffer()`.
