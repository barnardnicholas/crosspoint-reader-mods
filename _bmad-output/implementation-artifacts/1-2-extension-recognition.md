# Story 1.2: Extension Recognition

Status: review

## Story

As a user with DRM-free Kindle files on my SD card,
I want `.azw`, `.azw3`, and `.prc` files to appear in the file browser and open correctly,
so that I can read my existing Kindle library without a Calibre conversion step.

## Acceptance Criteria

1. `FsHelpers::hasMobiExtension()` returns `true` for filenames ending in `.azw`, `.azw3`, `.prc`, and `.mobi` (case-insensitive).
2. The file browser shows `.azw`, `.azw3`, and `.prc` files alongside `.mobi` files.
3. A DRM-free `.azw` file (which is structurally identical to MOBI7) opens and reads correctly.
4. `Mobi::getTitle()` title fallback correctly strips all four extensions (not just `.mobi`), returning the bare filename.
5. `pio run` produces zero errors and zero `-Wall` warnings.

## Tasks / Subtasks

- [x] Extend `hasMobiExtension()` (AC: #1, #2, #3)
  - [x] In `lib/FsHelpers/FsHelpers.cpp` line 81, replace:
    ```cpp
    bool hasMobiExtension(std::string_view fileName) { return checkFileExtension(fileName, ".mobi"); }
    ```
    with:
    ```cpp
    bool hasMobiExtension(std::string_view fileName) {
      return checkFileExtension(fileName, ".mobi") ||
             checkFileExtension(fileName, ".azw")  ||
             checkFileExtension(fileName, ".azw3") ||
             checkFileExtension(fileName, ".prc");
    }
    ```
  - [x] Update the docstring in `lib/FsHelpers/FsHelpers.h` for `hasMobiExtension()` to list all four extensions
- [x] Fix `Mobi::getTitle()` title fallback (AC: #4)
  - [x] In `lib/Mobi/Mobi.cpp` `getTitle()`, replaced hardcoded `-5` strip with `rfind('.')` — correct for all extensions
- [x] Verify build (AC: #5)
  - [x] Run `pio run` — confirm zero errors, zero warnings

## Dev Notes

**Callers of `hasMobiExtension()` — all inherit the change automatically, no modification needed:**
- `src/activities/home/FileBrowserActivity.cpp:98` — file browser visibility
- `src/activities/reader/ReaderActivity.cpp:26` — `isMobiFile()` routing gate
- `src/RecentBooksStore.cpp` — recent-books metadata scan
- `src/activities/boot_sleep/SleepActivity.cpp` — sleep/resume routing
- `lib/Mobi/Mobi.cpp` — `getTitle()` fallback (fixed in this story)

**Extension lengths:**
- `.mobi` = 5 chars → `substr(0, len-5)` was correct
- `.azw3` = 5 chars → same, but `.azw` = 4 and `.prc` = 4 → hardcoded `-5` was wrong
- Fix uses `rfind('.')` which is correct for all extensions

**DRM-free `.azw` files are structurally MOBI7** — once the extension is recognised, the existing `Mobi::load()` path handles them without any further changes. This is the "Phase 1" instant win from the research document.

**`checkFileExtension()` is case-insensitive** (uses `tolower` comparison at `FsHelpers.cpp:52`) — `.AZW`, `.Azw3` etc. all work.

**`std::string_view` safety** — `checkFileExtension` takes `string_view` which is safe here because it never passes `.data()` to a C API.

### Project Structure Notes

- Modified files: `lib/FsHelpers/FsHelpers.cpp`, `lib/FsHelpers/FsHelpers.h`, `lib/Mobi/Mobi.cpp`
- No new files

### References

- Architecture: Implementation Sequence item 2 — [architecture.md]
- Current `hasMobiExtension()`: `lib/FsHelpers/FsHelpers.cpp:81`
- Current `getTitle()` fallback: `lib/Mobi/Mobi.cpp:120-129`
- `checkFileExtension()` case-insensitivity: `lib/FsHelpers/FsHelpers.cpp:45-59`

## Dev Agent Record

### Agent Model Used
claude-sonnet-4-6

### Debug Log References

### Completion Notes List
- Extended `hasMobiExtension()` to return true for `.azw`, `.azw3`, `.prc` in addition to `.mobi`
- Updated header docstring accordingly
- Fixed `getTitle()` fallback: `rfind('.')` replaces hardcoded `length()-5` — correct for 4-char extensions (.azw, .prc)
- All 5 existing callers inherit the change with no modification
- Build: SUCCESS — zero errors, zero warnings

### File List
- lib/FsHelpers/FsHelpers.cpp
- lib/FsHelpers/FsHelpers.h
- lib/Mobi/Mobi.cpp
