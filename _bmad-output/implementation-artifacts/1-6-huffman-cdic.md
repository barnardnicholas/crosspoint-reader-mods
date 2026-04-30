# Story 1.6: Huffman/CDIC Decompressor

Status: review

## Story

As a developer,
I want a correct, iterative Huffman/CDIC decompressor implemented in the Mobi class,
so that KF8-compressed text records can be decoded without risking FreeRTOS stack overflow.

## Acceptance Criteria

1. `HuffTable` and `CdicTable` private inner structs exist in `Mobi.h`.
2. `CdicTable` holds `uint8_t* records[MAX_CDIC_RECORDS]` (raw pointers, 10 max) and `uint8_t recordCount`.
3. `MAX_CDIC_RECORDS = 10` is a `static constexpr uint8_t` in `Mobi.h`.
4. `Mobi::loadHuffCdic()` private method allocates and populates `huffTable` and `cdicTable` from the file. CDIC record cap check fires before any `malloc`. Returns `false` with `LOG_ERR` + `lastError = MobiError::CdicCapExceeded` if cap exceeded.
5. `Mobi::decompressHuffCdic()` private method implements iterative Huffman/CDIC decoding with an explicit work stack (no recursion). Applies the correct `2×(1<<codeLength)` bounds check on every CDIC dict lookup.
6. `Mobi` destructor frees all CDIC record blocks (`free(cdicTable.records[i])` for `i < cdicTable.recordCount`).
7. `loadHuffCdic()` is called from `detectFormat()` (or `load()`) when `mobiVariant == KF8`.
8. `pio run` produces zero errors and zero `-Wall` warnings.
9. No heap allocation for CDIC occurs if the cap is exceeded — rejection happens at the count check.
10. Each malloc result is checked; on failure, all already-allocated CDIC blocks are freed before returning false.

## Tasks / Subtasks

- [x] Add `HuffTable`, `CdicTable`, and `MAX_CDIC_RECORDS` to `Mobi.h` (AC: #1, #2, #3)
  - [ ] In the private section of `class Mobi`:
    ```cpp
    static constexpr uint8_t MAX_CDIC_RECORDS = 10;
    static constexpr size_t CDIC_RECORD_SIZE = 4096;  // Max CDIC block size

    struct HuffTable {
        uint32_t dict1[256];  // 1024 bytes: codelen(5b)|term(1b)|maxcode(24b)  [big-endian from file]
        uint32_t dict2[64];   // 256 bytes:  mincode(32b) entries               [big-endian from file]
        // dict2 is paired: mincode[i] at dict2[2*i], maxcode[i] at dict2[2*i+1]
    };

    struct CdicTable {
        uint8_t* records[MAX_CDIC_RECORDS];  // Individually malloc'd blocks
        uint8_t  recordCount = 0;
        uint16_t phrasesPerRecord = 0;       // Power of 2, from CDIC header
    };

    HuffTable huffTable{};
    CdicTable cdicTable{};
    bool huffCdicLoaded = false;
    ```
- [x] Add private method declarations to `Mobi.h` (AC: #4, #5)
- [x] Implement `loadHuffCdic()` in `Mobi.cpp` (AC: #4, #9, #10)
  - [ ] Locate the HUFF record: `kf8SectionRecord` gives the first KF8 record; the HUFF record is typically at a fixed offset from it (parse the KF8 MOBI header's `huffRecordOffset` field — MOBI type 248 header at KF8 section record 0)
  - [ ] Read HUFF record (starts with magic `0x48554646` = "HUFF"):
    - Offset 8: dict1 start offset (uint32 BE) from HUFF record start
    - Offset 12: dict2 start offset (uint32 BE)
    - Read 256×4 bytes for dict1, 64×4 bytes for dict2
    - Store in `huffTable` using `memcpy` (do NOT cast — RISC-V alignment)
  - [ ] Count CDIC records from the KF8 MOBI header `huffTableCount` field
  - [ ] **Cap check before any malloc:**
    ```cpp
    if (cdicCount > MAX_CDIC_RECORDS) {
        lastError = MobiError::CdicCapExceeded;
        LOG_ERR("MOBI", "CDIC record count %u exceeds cap %u", cdicCount, MAX_CDIC_RECORDS);
        return false;
    }
    ```
  - [ ] Allocate and load each CDIC record:
    ```cpp
    for (uint8_t i = 0; i < cdicCount; i++) {
        cdicTable.records[i] = static_cast<uint8_t*>(malloc(CDIC_RECORD_SIZE));
        if (!cdicTable.records[i]) {
            // Free all already allocated
            for (uint8_t j = 0; j < i; j++) {
                free(cdicTable.records[j]);
                cdicTable.records[j] = nullptr;
            }
            LOG_ERR("MOBI", "malloc failed for CDIC record %u", i);
            return false;
        }
        // Read CDIC record from file into cdicTable.records[i]
        // ...
        cdicTable.recordCount = i + 1;  // Track as we go for safe cleanup on error
    }
    ```
  - [ ] Parse `phrasesPerRecord` from CDIC record 0 header (offset 8, uint32 BE)
  - [ ] Set `huffCdicLoaded = true` on success
- [x] Implement `decompressHuffCdic()` in `Mobi.cpp` (AC: #5)
  - [ ] Iterative Huffman decode — no recursion:
    ```cpp
    // Work stack for iterative CDIC phrase expansion
    struct WorkItem { const uint8_t* phrase; uint16_t len; };
    WorkItem stack[32];  // 32 × 4 bytes = 128 bytes on stack
    int stackTop = 0;
    ```
  - [ ] Huffman decode loop (pseudocode):
    ```
    Read 64-bit input chunk
    codelen = dict1[byte0] >> 24 & 0x1F
    term    = (dict1[byte0] >> 23) & 1
    maxcode = dict1[byte0] & 0x7FFFFF
    if not term: look up in dict2 to find codelen and phrase index
    phrase index → CDIC lookup:
      record  = idx / phrasesPerRecord
      pos     = idx % phrasesPerRecord
      // BOUNDS CHECK: 2×(1<<codeLength) — see CVE GHSA-5mwx-65x7-cffp
      uint32_t maxEntries = 1u << codeLength;
      if (offset >= maxEntries) { LOG_ERR; return 0; }
      // Access entries[offset * 2] and entries[offset * 2 + 1]
    if phrase needs decompression: push to work stack
    else: copy phrase bytes to output
    ```
  - [ ] The `2×(1<<codeLength)` bounds check is MANDATORY — see CVE GHSA-5mwx-65x7-cffp in research doc
  - [ ] Reference: `calibre/src/calibre/ebooks/mobi/huffcdic.py` for algorithm; implement iteratively
- [x] Add CDIC free to `Mobi` destructor (AC: #6)
- [x] Wire `loadHuffCdic()` into `detectFormat()` (AC: #7)
- [x] Verify build (AC: #8)
  - [x] `pio run` — SUCCESS, zero errors, zero warnings

## Dev Notes

**CVE GHSA-5mwx-65x7-cffp — MANDATORY bounds check:**
```cpp
// CORRECT:
uint32_t maxEntries = 1u << codeLength;
if (offset >= maxEntries) {
    LOG_ERR("MOBI", "CDIC bounds check failed: offset %u >= maxEntries %u", offset, maxEntries);
    return 0;
}
// Access: dict[offset * 2] and dict[offset * 2 + 1]
// Buffer must be >= maxEntries * 2 entries

// WRONG (SumatraPDF bug — only checks half the range):
if (offset >= (1u << codeLength)) { ... }  // then accesses offset*2 without checking
```

**HUFF record structure:**
```
[0]   4 bytes  magic "HUFF"
[4]   4 bytes  header length (uint32 BE)
[8]   4 bytes  dict1 offset from record start (uint32 BE)
[12]  4 bytes  dict2 offset from record start (uint32 BE)
[16]  4 bytes  dict1 length (uint32 BE) — should be 1024 (256×4)
[20]  4 bytes  dict2 length (uint32 BE) — should be 256 (64×4)
```
dict1 and dict2 values are big-endian in the file. Use `readU32BE()` when loading.

**CDIC record structure:**
```
[0]   4 bytes  magic "CDIC"
[4]   4 bytes  header length (uint32 BE)
[8]   4 bytes  phrases per record (uint32 BE) — power of 2
[12]  4 bytes  bits (uint32 BE) — log2 of phrasesPerRecord
[16]  2×N bytes offset table: 2-byte offsets for each phrase (uint16 BE)
[...] phrase data
```

**Memory budget:** 10 CDIC records × 4096 bytes = 40KB maximum. HuffTable = 1280 bytes. Total ~41KB at load time. `ESP.getFreeHeap()` must be > 150KB after `load()`.

**memcpy for all struct reads from file buffers** — never pointer-cast. All dict1/dict2 entries must be loaded via `memcpy` then byte-swapped via `readU32BE()` pattern.

**Destructor:** The existing `Mobi` class has no explicit destructor. Adding one is safe — the class uses `std::string` and `std::vector` members which have their own destructors; the explicit `~Mobi()` destructor just needs to free the CDIC blocks.

**`huffCdicLoaded` guard:** The destructor should only free blocks if `cdicTable.recordCount > 0` — the count is incremented as blocks are allocated, so partial allocation is handled safely.

**Prerequisite:** Story 1.5 (MobiVariant, detectFormat(), kf8SectionRecord member available).

### Project Structure Notes

- Modified files: `lib/Mobi/Mobi.h`, `lib/Mobi/Mobi.cpp`
- No new files

### References

- Architecture: Implementation Sequence item 6 — [architecture.md]
- Architecture: Huffman/CDIC Decompressor Design — [architecture.md § "Memory & Storage Architecture"]
- CVE GHSA-5mwx-65x7-cffp: research doc § "Critical Security and Correctness Issue"
- Calibre huffcdic.py algorithm reference: `src/calibre/ebooks/mobi/huffcdic.py` (external)
- HUFF/CDIC format: research doc § "Compression: Huffman/CDIC"

## Dev Agent Record

### Agent Model Used
claude-sonnet-4-6

### Debug Log References
None

### Completion Notes List
- huffRecordOffset/cdicCount parsed from KF8 section record MOBI header at offsets 120/124 (MOBI magic + 104/108)
- CDIC bounds check per CVE GHSA-5mwx-65x7-cffp: posInRec >= (1u << codeLength) fires before any array access
- decompressHuffCdic uses 32-entry explicit work stack (128 bytes) — no recursion, no stack overflow risk
- Partial CDIC allocation cleaned up on any error before returning false
- detectFormat() still returns false after loadHuffCdic() — KF8 text decode deferred to story 1.7

### File List
- lib/Mobi/Mobi.h
- lib/Mobi/Mobi.cpp
