# Story 1.7: KF8 Text Record Parsing and readContent() Dispatch

Status: review

## Story

As a developer,
I want `Mobi::readContent()` to transparently handle KF8-compressed text records using the Huffman/CDIC decompressor built in story 1.6,
so that `MobiReaderActivity` can open and paginate AZW3 files without modification.

## Acceptance Criteria

1. The temporary `LOG_ERR + return false` stub for KF8 in `detectFormat()` (added in story 1.5) is removed.
2. `Mobi::load()` calls `loadHuffCdic()` when `mobiVariant == KF8`, then proceeds to build the virtual offset table.
3. `buildVirtualOffsetTable()` dispatches to `decompressHuffCdic()` for KF8 records (instead of `decompressPalmDoc()`).
4. `readContent()` dispatches to `decompressHuffCdic()` for KF8 records; `stripHtml()` is applied after decompression.
5. KF8 text record indices are correctly offset: the KF8 section starts at `kf8SectionRecord`; text records are at PalmDB indices `kf8SectionRecord + 1` through `kf8SectionRecord + textRecordCount` (using the KF8 section's `textRecordCount`, not the MOBI7 value).
6. `parseMobiHeaders()` is extended to re-parse the KF8 section header when `mobiVariant == KF8`: seek to the KF8 Record 0, re-read `compressionType`, `textRecordCount`, `maxRecordSize`, `extraDataFlags` from that record.
7. `ESP.getFreeHeap()` after `Mobi::load()` on a KF8 file: > 150KB (verified manually on device).
8. `pio run` produces zero errors and zero `-Wall` warnings.

## Tasks / Subtasks

- [x] Remove KF8 stub from `detectFormat()` (AC: #1)
  - [ ] In `Mobi.cpp`, find the block in `detectFormat()` (or `load()`) that was added in story 1.5:
    ```cpp
    if (mobiVariant == MobiVariant::KF8) {
        if (!loadHuffCdic()) return false;
        LOG_ERR("MOBI", "KF8 text reading not yet implemented");
        return false;
    }
    ```
  - [ ] Replace with the permanent KF8 dispatch (wired in subsequent tasks below)

- [x] Extend `parseMobiHeaders()` to re-parse KF8 section header (AC: #5, #6)
  - [ ] After setting `mobiVariant = MobiVariant::KF8` (when EXTH 121 is found or MOBI type == 248), re-read the KF8 section's Record 0:
    ```cpp
    // kf8SectionRecord is now set (from EXTH 121)
    // Validate kf8SectionRecord is within bounds
    if (kf8SectionRecord == 0xFFFFFFFF || kf8SectionRecord >= recordFileOffsets.size()) {
        LOG_ERR("MOBI", "Invalid KF8 section record %u", kf8SectionRecord);
        return false;
    }
    // Seek to KF8 Record 0 (the record immediately after the boundary marker)
    const uint32_t kf8Rec0Offset = recordFileOffsets[kf8SectionRecord];
    // Re-read up to REC0_READ_SIZE bytes from KF8 Record 0
    // Parse: compressionType, textRecordCount, maxRecordSize, extraDataFlags
    // from KF8 Record 0 (same offsets as MOBI7 rec0)
    // Also read huffRecordOffset from KF8 MOBI header (for loadHuffCdic)
    ```
  - [ ] Store `kf8FirstTextRecord = kf8SectionRecord + 1` as a private member for use in `readRawRecord()`
  - [ ] Add `uint32_t kf8FirstTextRecord = 1;` to `Mobi.h` private section (default 1 = MOBI7 behaviour)
  - [ ] Add `uint16_t huffRecordOffset = 0;` to `Mobi.h` private section (offset into KF8 section for HUFF record)

- [x] Update `readRawRecord()` to use correct KF8 record indices (AC: #5)
  - [ ] Current code: `const uint16_t palmRecIdx = recIdx + 1;`
  - [ ] Change to: `const uint32_t palmRecIdx = kf8FirstTextRecord + recIdx;`
    - For MOBI7: `kf8FirstTextRecord = 1` → behaviour unchanged
    - For KF8: `kf8FirstTextRecord = kf8SectionRecord + 1` → accesses KF8 text records
  - [ ] Update the type from `uint16_t` to `uint32_t` to handle large record indices

- [x] Wire `loadHuffCdic()` into `load()` (AC: #2)
  - [ ] In `Mobi::load()`, after `loadHeader()` succeeds:
    ```cpp
    if (mobiVariant == MobiVariant::KF8) {
        if (!loadHuffCdic()) return false;
    }
    ```
  - [ ] `loadHuffCdic()` already declared and implemented in story 1.6; this wires it into the load sequence

- [x] Extend `buildVirtualOffsetTable()` for KF8 (AC: #3)
  - [ ] Current decompression dispatch:
    ```cpp
    if (compressionType == COMPRESSION_PALMDOC) {
        strippedLen = decompressPalmDoc(rawBuf, rawSize, decompBuf, decompBufSize);
    } else {
        memcpy(decompBuf, rawBuf, rawSize);
        strippedLen = rawSize;
    }
    ```
  - [ ] Add KF8 branch:
    ```cpp
    if (compressionType == COMPRESSION_HUFFMAN) {
        strippedLen = decompressHuffCdic(rawBuf, rawSize, decompBuf, decompBufSize);
        if (strippedLen == 0) {
            LOG_ERR("MOBI", "HuffCdic decompression failed on record %u", r);
            // Continue: skip this record rather than aborting the whole table build
        }
    } else if (compressionType == COMPRESSION_PALMDOC) {
        strippedLen = decompressPalmDoc(rawBuf, rawSize, decompBuf, decompBufSize);
    } else {
        memcpy(decompBuf, rawBuf, rawSize);
        strippedLen = rawSize;
    }
    ```

- [x] Extend `readContent()` for KF8 (AC: #4)
  - [ ] Same dispatch pattern as `buildVirtualOffsetTable()`:
    ```cpp
    size_t strippedLen = 0;
    if (compressionType == COMPRESSION_HUFFMAN) {
        strippedLen = decompressHuffCdic(rawBuf, rawSize, decompBuf, decompBufSize);
    } else if (compressionType == COMPRESSION_PALMDOC) {
        strippedLen = decompressPalmDoc(rawBuf, rawSize, decompBuf, decompBufSize);
    } else {
        if (rawSize <= decompBufSize) {
            memcpy(decompBuf, rawBuf, rawSize);
            strippedLen = rawSize;
        }
    }
    // stripHtml() follows — no change needed
    ```

- [x] Verify build (AC: #8)
  - [x] `pio run` — SUCCESS, zero errors, zero warnings

## Dev Notes

**KF8 dual-format structure:** An AZW3 file is a compound PalmDB: a full MOBI7 section (records 0..N) followed by a BOUNDARY record, then the KF8 section (records N+2..end). EXTH record 121 gives the index of the BOUNDARY record. The KF8 text records begin at `kf8SectionRecord + 1`.

**KF8 Record 0 re-parse:** The KF8 section has its own Record 0 (immediately after the boundary) with a fresh MOBI header (type=248). This header has its own `textRecordCount`, `compressionType` (17480), `maxRecordSize`, and `extraDataFlags`. The MOBI7 values from the original Record 0 are NOT used for KF8 text access. After KF8 detection, `parseMobiHeaders()` must re-seek and re-read the KF8 Record 0 to populate these fields correctly.

**`huffRecordOffset` from KF8 MOBI header:** The KF8 MOBI header contains a field at offset 112 (relative to the KF8 Record 0 MOBI magic) giving the record index of the HUFF record relative to the KF8 section Record 0. `loadHuffCdic()` (story 1.6) needs this to locate the HUFF record.

**`readRawRecord()` index change:** The existing `const uint16_t palmRecIdx = recIdx + 1` assumes MOBI7 layout (text records start at PalmDB record 1). For KF8, text records start at `kf8SectionRecord + 1`. Changing to `uint32_t palmRecIdx = kf8FirstTextRecord + recIdx` handles both cases when `kf8FirstTextRecord` defaults to 1.

**No skeleton+FRAG needed:** KF8 text records contain inline XHTML. The skeleton/FRAG reassembly (used by KindleUnpack) is only needed to reconstruct discrete EPUB-like files. For flat-text extraction via `stripHtml()`, sequential decompression of text records suffices. Skip FDST parsing entirely.

**decompBuf size for KF8:** KF8 text records can expand more than PalmDOC. Use `maxRecordSize * 4 + 32` for decompBuf (same as current) — KF8 records are bounded by maxRecordSize from the KF8 header, so this is safe.

**`huffCdicLoaded` guard:** `decompressHuffCdic()` should check `huffCdicLoaded` and return 0 if called before `loadHuffCdic()` succeeds.

**FDST record:** KF8 files may have an FDST record describing flow boundaries (text, CSS, images). For this implementation, ignore FDST — treat all records 1..textRecordCount as text. CSS and image records appear after the text records and are not reached by the virtual offset table builder (which stops at `textRecordCount`).

### Project Structure Notes

- Modified files: `lib/Mobi/Mobi.h`, `lib/Mobi/Mobi.cpp`
- No new files

### References

- Architecture: Implementation Sequence item 7 — [architecture.md]
- Architecture: KF8 Parser Design — [architecture.md § "KF8 Parser Design"]
- Architecture: Content Access Sequence — [architecture.md § "Content Access Sequence"]
- Research doc: KF8 Parser Design § "Initialization Sequence" and "Content Access Sequence"
- Research doc: Design Principles — "skeleton+FRAG: Skip for v1"
- `readContent()`: `lib/Mobi/Mobi.cpp:155`
- `buildVirtualOffsetTable()`: `lib/Mobi/Mobi.cpp:513`
- `readRawRecord()`: `lib/Mobi/Mobi.cpp:657`
- Prerequisite: Story 1.6 (loadHuffCdic, decompressHuffCdic implemented)

## Dev Agent Record

### Agent Model Used
claude-sonnet-4-6

### Debug Log References
None

### Completion Notes List
- KF8 rec0 re-parse done at end of parseMobiHeaders() (while file handle still open), not in detectFormat()
- kf8FirstTextRecord member (default 1) handles MOBI7/KF8 dispatch in readRawRecord() with no branching
- loadHuffCdic() moved from detectFormat() to load() so VoT build follows it in the correct order
- readRawRecord() palmRecIdx changed from uint16_t to uint32_t to handle large KF8 record indices
- decompressHuffCdic dispatch added to both buildVirtualOffsetTable() and readContent()

### File List
- lib/Mobi/Mobi.h
- lib/Mobi/Mobi.cpp
