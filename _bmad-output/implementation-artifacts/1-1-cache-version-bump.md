# Story 1.1: Cache Version Bump

Status: review

## Story

As a developer adding KF8 support,
I want the virtual offset cache version bumped to 2 before any other changes land,
so that existing MOBI7 caches are silently invalidated and rebuilt rather than misread as v2 caches.

## Acceptance Criteria

1. `VOFFSET_VERSION` constant in `lib/Mobi/Mobi.cpp` is changed from `1` to `2`.
2. `pio run` produces zero errors and zero `-Wall` warnings.
3. Opening an existing `.mobi` file (that has a v1 `voffsets.bin` cache) triggers a cache rebuild (the old cache is rejected because `version != VOFFSET_VERSION`).
4. No functional change to reading behaviour for existing MOBI files.

## Tasks / Subtasks

- [x] Change `VOFFSET_VERSION` (AC: #1)
  - [x] In `lib/Mobi/Mobi.cpp` anonymous namespace, change `constexpr uint8_t VOFFSET_VERSION = 1;` to `= 2;`
- [x] Verify build (AC: #2)
  - [x] Run `pio run` — confirm zero errors, zero warnings
- [x] Manual verification note (AC: #3, #4)
  - [x] Add a dev note confirming: on next open of any `.mobi` file, the old v1 cache will be rejected (`version != VOFFSET_VERSION` at `Mobi.cpp` load check) and `buildVirtualOffsetTable()` will run instead

## Dev Notes

**Why this is story 1:** The v2 cache format (with new KF8 fields) will be written by later stories. If v1 caches from MOBI7 files were still present when v2 write logic lands, there would be no version mismatch — the old cache would be misread. Bumping the version first guarantees a clean state throughout development.

**Exact change location:**
```cpp
// lib/Mobi/Mobi.cpp — anonymous namespace (line ~14)
constexpr uint8_t VOFFSET_VERSION = 2;  // was: 1
```

**Cache invalidation path** (already implemented — no code change needed here):
- `Mobi::loadVirtualOffsetTable()` reads `version` from `voffsets.bin`
- Compares to `VOFFSET_VERSION` — mismatch → returns `false` → `buildVirtualOffsetTable()` runs
- `buildVirtualOffsetTable()` calls `saveVirtualOffsetTable()` which writes the new version number

**MOBI7 cache rebuild is transparent** — user sees no difference; first open after firmware flash is slightly slower (rebuild), subsequent opens are fast.

**Do NOT change** `loadVirtualOffsetTable()` or `saveVirtualOffsetTable()` serialisation format in this story — that is story 1.9.

### Project Structure Notes

- Only file modified: `lib/Mobi/Mobi.cpp`
- No header changes, no new files

### References

- Architecture: Implementation Sequence item 1 — [architecture.md]
- Cache format: `lib/Mobi/Mobi.cpp` `loadVirtualOffsetTable()` / `saveVirtualOffsetTable()`

## Dev Agent Record

### Agent Model Used
claude-sonnet-4-6

### Debug Log References

### Completion Notes List
- Changed `VOFFSET_VERSION` from 1 to 2 at `lib/Mobi/Mobi.cpp:14`
- Build: SUCCESS — zero errors, zero warnings (`pio run`)
- Cache invalidation path already implemented: `loadVirtualOffsetTable()` version mismatch → returns false → `buildVirtualOffsetTable()` runs

### File List
- lib/Mobi/Mobi.cpp
