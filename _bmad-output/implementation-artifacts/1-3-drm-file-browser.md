# Story 1.3: DRM File Browser Indicator

Status: review

## Story

As a user with DRM-protected Kindle files on my SD card,
I want DRM-locked files to be visually marked in the file browser,
so that I know before selecting them that they cannot be opened.

## Acceptance Criteria

1. `Mobi::isDrmLocked(const char* path)` static method exists and returns `true` for files with `encryptionType == 2` (PalmDOC header offset 12 of Record 0), `false` otherwise.
2. The method opens only enough of the file to read the Record 0 offset and the first ~100 bytes of Record 0. It does NOT construct a `Mobi` instance.
3. In `FileBrowserActivity::render()`, the `rowValue` callback shows a DRM indicator string (e.g., `"[DRM]"`) for `.azw`/`.azw3`/`.prc` files that are locked.
4. Non-DRM-locked Kindle files and non-Kindle files show no value in the `rowValue` column.
5. `pio run` produces zero errors and zero `-Wall` warnings.
6. The file browser still renders correctly for all existing file types (regression check).

## Tasks / Subtasks

- [x] Add `isDrmLocked()` to `Mobi.h` (AC: #1, #2)
  - [x] Added `[[nodiscard]] static bool isDrmLocked(const char* path);` to public section of `class Mobi`
- [x] Implement `isDrmLocked()` in `Mobi.cpp` (AC: #1, #2)
  - [x] Implemented after constructor; reads PalmDB header, seeks to Record 0, checks encryptionType at offset 12
- [x] Update `FileBrowserActivity::render()` (AC: #3, #4, #6)
  - [x] Added `#include <Mobi.h>` to `FileBrowserActivity.cpp`
  - [x] Added `rowValue` lambda to `GUI.drawList()` — returns "[DRM]" for .azw/.azw3/.prc if DRM-locked, "" otherwise
- [x] Verify build (AC: #5)
  - [x] Run `pio run` — SUCCESS, zero errors, zero warnings

## Dev Notes

**`isDrmLocked()` is a static method** — it does not construct or use a `Mobi` instance. It shares the `readU16BE()` / `readU32BE()` helpers and the `PALMDB_HEADER_SIZE` / `PALMDB_RECORD_COUNT_OFFSET` constants which are in the anonymous namespace of `Mobi.cpp`. This is fine — it's implemented in the same translation unit.

**`checkFileExtension()` is in the `FsHelpers` namespace** — ensure `FsHelpers::checkFileExtension` is declared in `FsHelpers.h` (check if it's currently only declared in the .cpp). If it's not in the header, use `hasMobiExtension()` + a check for `.mobi` to determine Kindle-specific files.

**Performance concern:** `isDrmLocked()` is called once per `.azw`/`.azw3`/`.prc` file during directory render. This involves an SD card open + ~186 byte read per file. For a directory with many Kindle files this adds latency. Acceptable for MVP — the alternative (caching DRM status) is out of scope.

**`basepath` path construction:** Looking at `FileBrowserActivity.cpp`, `basepath` may or may not have a trailing slash. The `fullPath` construction above guards for this. The `files` vector contains bare filenames (not full paths).

**Do NOT call `isDrmLocked()` for `.mobi` files** — they are MOBI7 format and the existing code already handles them. Only Kindle-specific extensions (`.azw`, `.azw3`, `.prc`) may be DRM-locked in practice.

**`rowValue` in `drawList`** — Looking at `BaseTheme.h:118`, `drawList` already supports a `rowValue` callback parameter (currently `nullptr` in `FileBrowserActivity`). The value string appears on the right side of each list row. `"[DRM]"` is sufficient; no new UI components needed.

**Regression check:** All non-MOBI file types return `""` from the rowValue callback — no visual change for EPUB, XTC, TXT, BMP files.

### Project Structure Notes

- Modified files: `lib/Mobi/Mobi.h`, `lib/Mobi/Mobi.cpp`, `src/activities/home/FileBrowserActivity.cpp`
- New includes may be needed in `FileBrowserActivity.cpp`

### References

- Architecture: Implementation Sequence item 3 — [architecture.md]
- `drawList` signature: `src/components/themes/BaseTheme.h:118`
- `FileBrowserActivity::render()`: `src/activities/home/FileBrowserActivity.cpp:248-275`
- PalmDOC `encryptionType` at Record 0 offset 12: research doc § "Integration Patterns Analysis — DRM Detection"
- `basepath` + `files[index]` full path: `FileBrowserActivity.cpp:183-191`

## Dev Agent Record

### Agent Model Used
claude-sonnet-4-6

### Debug Log References

### Completion Notes List
- `Mobi::isDrmLocked()` static method added to `Mobi.h` and implemented in `Mobi.cpp`
- Uses existing anonymous-namespace constants (PALMDB_HEADER_SIZE, PALMDB_RECORD_COUNT_OFFSET, PALMDB_RECORD_ENTRY_SIZE, readU16BE, readU32BE)
- `FileBrowserActivity` now shows "[DRM]" in rowValue column for DRM-locked .azw/.azw3/.prc files
- .mobi files not checked (MOBI7 — assumed safe)
- Build: SUCCESS — zero errors, zero warnings

### File List
- lib/Mobi/Mobi.h
- lib/Mobi/Mobi.cpp
- src/activities/home/FileBrowserActivity.cpp
