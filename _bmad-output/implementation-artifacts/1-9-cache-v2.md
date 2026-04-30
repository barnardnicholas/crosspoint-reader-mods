# Story 1.9: voffsets.bin v2 Cache Header Serialization

Status: ready-for-dev

## Story

As a developer,
I want `voffsets.bin` to persist the KF8-specific metadata fields (`mobiVariant`, `kf8SectionRecord`, `cdicCount`) alongside the existing fields,
so that subsequent opens of an AZW3 file are fast (no rebuild) and stale MOBI7 caches are correctly invalidated.

## Acceptance Criteria

1. `VOFFSET_VERSION` is `2` (bumped from `1` in story 1.1; this story ensures the v2 header fields are actually written and read).
2. `saveVirtualOffsetTable()` writes three new fields after the existing header fields: `uint8_t mobiVariant`, `uint32_t kf8SectionRecord` (little-endian), `uint8_t cdicCount`.
3. `loadVirtualOffsetTable()` reads and validates all three new fields; a mismatch on any field triggers a cache rebuild (returns `false`).
4. `loadVirtualOffsetTable()` resets `progress.bin` to offset 0 when cache is rebuilt due to v1→v2 version mismatch (to prevent stale page-number from mapping to wrong content after rebuild).
5. An existing v1 cache file is rejected by the version check (returns false → rebuilds) — no crash, no data corruption.
6. A v2 MOBI7 cache round-trips correctly: load → save → load produces identical virtual offsets. `kf8SectionRecord` = 0xFFFFFFFF, `cdicCount` = 0.
7. A v2 KF8 cache round-trips correctly: `mobiVariant` = 1, `kf8SectionRecord` = correct record index, `cdicCount` = correct count.
8. `pio run` produces zero errors and zero `-Wall` warnings.

## Tasks / Subtasks

- [ ] Confirm `VOFFSET_VERSION = 2` (AC: #1)
  - [ ] Story 1.1 bumped the version. Verify `lib/Mobi/Mobi.cpp` line 14 reads `constexpr uint8_t VOFFSET_VERSION = 2;`
  - [ ] If not yet bumped (story 1.1 not landed), change it now

- [ ] Update `saveVirtualOffsetTable()` to write v2 header fields (AC: #2)
  - [ ] In `Mobi.cpp`, `saveVirtualOffsetTable()` currently writes:
    ```
    magic (4) | version (1) | fileSize (4) | textRecordCount (2) | compressionType (2) | extraDataFlags (2)
    ```
  - [ ] After `extraDataFlags`, add:
    ```cpp
    const uint8_t variantByte = static_cast<uint8_t>(mobiVariant);
    f.write(&variantByte, 1);
    f.write(reinterpret_cast<const uint8_t*>(&kf8SectionRecord), 4);  // little-endian
    f.write(&cdicTable.recordCount, 1);
    ```
  - [ ] Total v2 header size: 4+1+4+2+2+2+1+4+1 = **21 bytes** (vs 15 bytes for v1)
  - [ ] The virtualOffsets[] array follows immediately after

- [ ] Update `loadVirtualOffsetTable()` to read and validate v2 header fields (AC: #3, #5)
  - [ ] After reading the existing 6 header fields, read the 3 new v2 fields:
    ```cpp
    uint8_t cachedVariant = 0;
    uint32_t cachedKf8Section = 0;
    uint8_t cachedCdicCount = 0;
    if (f.read(&cachedVariant, 1) != 1 ||
        f.read(&cachedKf8Section, 4) != 4 ||
        f.read(&cachedCdicCount, 1) != 1) {
        f.close();
        return false;
    }
    ```
  - [ ] Add to the validation check:
    ```cpp
    if (cachedVariant != static_cast<uint8_t>(mobiVariant) ||
        cachedKf8Section != kf8SectionRecord ||
        cachedCdicCount != cdicTable.recordCount) {
        LOG_DBG("MOBI", "Virtual offset cache KF8 fields mismatch — will rebuild");
        f.close();
        return false;
    }
    ```
  - [ ] Version check already handles v1→v2 mismatch (version byte != `VOFFSET_VERSION` → return false)

- [ ] Reset `progress.bin` on cache rebuild triggered by version mismatch (AC: #4)
  - [ ] In `Mobi::load()`, when `loadVirtualOffsetTable()` returns false:
    ```cpp
    if (!loadVirtualOffsetTable()) {
        // Reset progress on cache rebuild — stale page number may not map
        // correctly to rebuilt virtual offsets (especially v1→v2 upgrade)
        resetProgressCache();
        if (!buildVirtualOffsetTable()) {
            LOG_ERR("MOBI", "Failed to build virtual offset table");
            return false;
        }
        saveVirtualOffsetTable();
    }
    ```
  - [ ] Add private method `void resetProgressCache() const`:
    ```cpp
    void Mobi::resetProgressCache() const {
        const std::string progressFile = cachePath + "/progress.bin";
        if (!Storage.exists(progressFile.c_str())) return;
        FsFile f;
        if (Storage.openFileForWrite("MOBI", progressFile, f)) {
            const uint8_t zero[4] = {0, 0, 0, 0};
            f.write(zero, 4);
            f.close();
            LOG_DBG("MOBI", "Reset progress cache after voffsets rebuild");
        }
    }
    ```
  - [ ] Declare in `Mobi.h` private section: `void resetProgressCache() const;`

- [ ] Document format in `docs/file-formats.md` (AC: informational)
  - [ ] Find the existing `voffsets.bin` documentation in `docs/file-formats.md`
  - [ ] Update or add the v2 layout table:
    ```
    voffsets.bin v2 layout (all values little-endian):
    [0]   uint32_t  magic            = 0x4D424F49 ("MBOI")
    [4]   uint8_t   version          = 2
    [5]   uint32_t  fileSize
    [9]   uint16_t  textRecordCount
    [11]  uint16_t  compressionType
    [13]  uint16_t  extraDataFlags
    [15]  uint8_t   mobiVariant      0=MOBI7, 1=KF8
    [16]  uint32_t  kf8SectionRecord 0xFFFFFFFF if MOBI7
    [20]  uint8_t   cdicCount        0 if not KF8
    [21]  uint32_t  virtualOffsets[textRecordCount+1]
    ```

- [ ] Verify build (AC: #8)
  - [ ] Run `pio run` — confirm zero errors, zero warnings

## Dev Notes

**Little-endian vs big-endian in cache:** The existing cache writes struct fields directly via `f.write(reinterpret_cast<const uint8_t*>(&field), size)` — this writes native (little-endian on ESP32-C3 RISC-V) byte order. The new fields follow the same pattern. Do NOT byte-swap these values. They are read back on the same device.

**Why reset progress.bin:** The v1 cache stored page-based progress (`progress.bin` has `currentPage` as a 2-byte integer in `MobiReaderActivity`). After a virtual offset table rebuild (even for the same file), the page count may differ because `textRecordCount` or `compressionType` changed (e.g., MOBI7→KF8 re-detection). Rather than computing a mapping, reset to page 0. This is a one-time inconvenience per file on the device upgrade.

**`cdicTable.recordCount` availability:** `loadHuffCdic()` must be called before `saveVirtualOffsetTable()` (which it is, since `load()` calls `loadHuffCdic()` before `buildVirtualOffsetTable()`). For MOBI7 files, `cdicTable.recordCount` is 0 (default).

**v1 cache rejection:** The `version != VOFFSET_VERSION` check in `loadVirtualOffsetTable()` already handles this. No extra code needed — a v1 cache has version byte `1`, current `VOFFSET_VERSION` is `2`, mismatch → returns false → rebuild.

**`resetProgressCache()` is non-critical:** If the progress reset fails (e.g., file doesn't exist), `LOG_DBG` and continue — it's a convenience, not a correctness requirement. The reader's `loadProgress()` in `MobiReaderActivity` already clamps to valid page range.

**Field order immutability:** Once v2 ships to a device, the field order is frozen. Never reorder fields in a subsequent change — bump the version instead.

### Project Structure Notes

- Modified files: `lib/Mobi/Mobi.h`, `lib/Mobi/Mobi.cpp`, `docs/file-formats.md`
- No new source files

### References

- Architecture: Implementation Sequence item 9 — [architecture.md]
- Architecture: voffsets.bin v2 header layout — [architecture.md § "SD Card Cache Structure"]
- Architecture gap: "progress.bin stores a virtual byte offset…reset progress.bin to 0 when cache rebuild triggered" — [architecture.md § "Gap Analysis"]
- Research doc: Cache Architecture § "voffsets.bin format v2"
- `saveVirtualOffsetTable()`: `lib/Mobi/Mobi.cpp:629`
- `loadVirtualOffsetTable()`: `lib/Mobi/Mobi.cpp:580`
- `Mobi::load()`: `lib/Mobi/Mobi.cpp:69`
- Prerequisite: Story 1.1 (VOFFSET_VERSION bumped to 2), Story 1.5 (mobiVariant member), Story 1.6 (cdicTable.recordCount populated)

## Dev Agent Record

### Agent Model Used

### Debug Log References

### Completion Notes List

### File List
