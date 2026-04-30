---
stepsCompleted: [1, 2, 3, 4, 5, 6, 7, 8]
lastStep: 8
status: 'complete'
completedAt: '2026-04-27'
inputDocuments:
  - _bmad-output/planning-artifacts/prd.md
  - _bmad-output/planning-artifacts/research/technical-azw-azw3-mobi-support-research-2026-04-27.md
  - docs/contributing/architecture.md
  - docs/fork-decisions.md
workflowType: 'architecture'
project_name: 'crosspoint-reader-mods'
user_name: 'Nick'
date: '2026-04-27'
---

# Architecture Decision Document

_This document builds collaboratively through step-by-step discovery. Sections are appended as we work through each architectural decision together._

## Project Context Analysis

### Requirements Overview

**Functional Requirements (26 total):**

- **File Discovery & Recognition (FR1–3)**: `.azw`, `.azw3`, `.prc` extension support in FsHelpers; DRM-visual indicator in file browser; fast DRM check (header-only, no decompression) for metadata scans.
- **Format Detection & Parsing (FR4–7)**: PalmDB container parsing for all Kindle variants; KF8 compound-file boundary detection via EXTH record 121; KF8 section header parsing; EXTH metadata extraction (title, author).
- **Content Decompression (FR8–11)**: PalmDOC (type 2) reused unchanged; Huffman/CDIC (type 17480) new iterative implementation; CDIC table loading; hard cap at 10 CDIC records before any heap allocation.
- **Text Extraction & Virtual Access (FR12–16)**: HTML strip → plain text; virtual flat-text interface via byte offset; virtual offset table construction; SD cache (`voffsets.bin` v2); stale-cache detection and rebuild.
- **Chapter Navigation (FR17–19)**: TOC from KF8 INDX records; chapter list UI (existing MOBI chapter selection pattern); direct navigation by byte offset.
- **Reading & Progress (FR20–22)**: Pagination via existing layout pipeline (unchanged); progress save/restore by virtual byte offset.
- **Error Handling & Rejection (FR23–26)**: DRM at `loadHeader()` with no decompression; user-facing `FullScreenMessageActivity` error for DRM and CDIC-cap exceeded; malformed PalmDB graceful rejection.

**Non-Functional Requirements:**

- **Performance**: Page-turn ≤ 100ms (SD→render); first-open ≤ 5s for ≤ 500 text records; subsequent opens (cached) indistinguishable from MOBI7.
- **Memory Safety**: `ESP.getFreeHeap()` > 150KB after `Mobi::load()` on KF8; reader task stack > 512 bytes headroom; no fragmentation accumulation over 20+ page turns; CDIC allocation hard-capped (10 records ≈ 40KB max).
- **Reliability**: Zero crashes on device during full read session; all error paths return `false` + `LOG_ERR`, never `abort()`; clean state on all rejection paths; zero compiler warnings.

**Scale & Complexity:**

- Primary domain: IoT/Embedded firmware (ESP32-C3, C++20, PlatformIO, FreeRTOS)
- Complexity level: Medium — isolated extension to a single class (`Mobi`), bounded scope, one new algorithm
- Estimated architectural components: 5 (Huffman/CDIC decompressor, KF8 parser, virtual offset table v2, DRM detection, extension recognition)

### Technical Constraints & Dependencies

- **380KB DRAM, no PSRAM** — hard ceiling; every allocation must be justified and bounded
- **FreeRTOS task stack 2048 bytes** — eliminates recursive Huffman decompression; iterative-only
- **Single-buffer e-ink framebuffer (48KB)** — no display change required, but 48KB is pre-committed from RAM budget
- **HalStorage / SdFat FsFile API** — no POSIX, no `fopen`; all file I/O through existing HAL
- **C++20, no exceptions, no RTTI** — error propagation via `bool` return + `LOG_ERR`; no `std::exception`
- **PlatformIO build chain** — no new external libraries permitted; Huffman/CDIC implemented natively
- **Known CVE GHSA-5mwx-65x7-cffp** — `2×(1<<codeLength)` bounds check mandatory from first implementation

### Cross-Cutting Concerns Identified

- **Memory budget** — CDIC heap allocation, working buffers, virtual offset table all compete for the same 380KB pool; must be measured throughout development
- **Error handling uniformity** — DRM rejection, CDIC-cap rejection, and malformed-header rejection all funnel to `FullScreenMessageActivity`; `ReaderActivity::onEnter()` already handles `Mobi::load()` returning false
- **Cache invalidation** — `voffsets.bin` version bump must be the first code change; any MOBI7 regression requires immediate detection
- **Format detection propagation** — `hasMobiExtension()` changes propagate to 5 callers; DRM visual state must propagate to file browser without coupling it to reader internals
- **Format-transparent interface contract** — `Mobi::readContent(offset, length)` must remain stable; KF8 complexity must not leak above the `Mobi` class boundary

## Technology Foundation

### Primary Technology Domain

**IoT/Embedded Firmware** — brownfield extension to an existing PlatformIO project.
No starter template applies; the technology stack is fully established by the existing codebase.

### Established Technology Stack

**Language & Runtime:**
- C++20 (`-std=c++2a`), no exceptions (`-fno-exceptions`), no RTTI (`-fno-rtti`)
- ESP32-C3 RISC-V target via PlatformIO + Arduino-ESP32 framework
- FreeRTOS for task management (single-core, cooperative-ish scheduling)

**Build Tooling:**
- PlatformIO (`platformio.ini`) — `default` (dev), `gh_release` (prod), `gh_release_rc` (RC), `slim` (minimal)
- `pio run` for build; `pio run -t upload` for flash
- Logging via `LOG_INF/LOG_DBG/LOG_ERR` from `Logging.h` (not `Serial`)

**Storage & I/O:**
- HalStorage (`Storage` singleton) wrapping SdFat `FsFile` — no POSIX, no `fopen`
- SD card for books, caches (`.crosspoint/`), settings

**Error Handling:**
- `bool` return + `LOG_ERR` pattern — no exceptions, no `abort()`
- `FullScreenMessageActivity` for user-visible errors

**Code Organization:**
- `lib/Mobi/` — all format complexity lives here; reader pipeline above is format-agnostic
- `src/activities/reader/` — `MobiReaderActivity` unchanged by this feature
- `lib/FsHelpers/` — extension detection (`hasMobiExtension()`)

**Note:** No initialization command. Feature branch `FEAT-azw-azw-3-support` already exists.

## Core Architectural Decisions

### Decision Priority Analysis

**Critical Decisions (Block Implementation):**
- Iterative Huffman decoder (no recursion) — FreeRTOS stack overflow prevention
- `2×(1<<codeLength)` CDIC bounds check — CVE mitigation, mandatory from first commit
- `voffsets.bin` v2 format bump — must land before any KF8 cache writes
- `detectFormat()` separation — establishes the dispatch point for all format-specific paths

**Important Decisions (Shape Architecture):**
- CDIC array-of-blocks memory model
- `MobiVariant` enum for format dispatch
- `Mobi::isDrmLocked()` static helper for file browser
- New i18n keys for DRM and CDIC-cap errors
- `parseToc()` populates existing chapter struct type

**Deferred Decisions (Post-MVP):**
- Cover art extraction from KF8 image records (Growth phase; sidecar fallback sufficient for MVP)
- KFX/AZW8 support (blocked on spec; no timeline)

---

### Memory & Storage Architecture

**CDIC Table Memory Model: Array of Blocks**
- `CdicTable` holds `uint8_t* records[MAX_CDIC_RECORDS]` (10 raw pointers) + `uint8_t recordCount`
- Each CDIC block is individually `malloc`'d in `loadHuffCdic()` — one allocation per record (~4096 bytes each)
- All blocks freed in `Mobi` destructor (RAII; `Mobi` lifetime = `MobiReaderActivity` lifetime)
- Rationale: avoids a single large contiguous allocation (harder to satisfy on fragmented heap); individual blocks fail predictably at the cap boundary

**Virtual Offset Table: voffsets.bin v2**
- Extended `VOffsetCacheHeader` with: `mobiVariant` (uint8_t), `kf8SectionRecord` (uint32_t), `cdicCount` (uint8_t)
- Version bump from 1 → 2 invalidates all existing MOBI7 caches silently; rebuild is transparent on first open
- Existing MOBI7 cache rebuild path unchanged; no data loss

---

### Class Interface & Format Dispatch

**MobiVariant Enum**
```cpp
enum class MobiVariant : uint8_t { MOBI7 = 0, KF8 = 1 };
```
Stored as private member `mobiVariant` in `Mobi`. Defaulted to `MOBI7`.

**detectFormat() Separation**
- Private method `detectFormat()` called from `load()` after PalmDB record offsets are parsed, before any decompressor initialization
- Reads MOBI type field and EXTH record 121; sets `mobiVariant` and `kf8SectionRecord`
- `loadHeader()` remains a lower-level record reader; format identity lives one level up in `detectFormat()`
- Rationale: keeps `loadHeader()` focused on binary parsing; `detectFormat()` owns the semantic branch

**readContent() Contract — Unchanged**
- `Mobi::readContent(uint8_t* buf, uint32_t offset, uint32_t length)` signature and semantics preserved
- KF8 decompression dispatches internally on `mobiVariant`; caller (MobiReaderActivity) is format-agnostic

**parseToc() — Chapter Navigation**
- Private method `parseToc()` called from `load()` on KF8 path when INDX records are present
- Populates existing chapter struct type (same type used by existing MOBI chapter selection UI)
- Exposed via existing getter pattern; `MobiReaderActivity` chapter selection UI requires no changes

---

### Security & Error Handling

**DRM Detection: Mobi::isDrmLocked() Static Helper**
- `static bool Mobi::isDrmLocked(const char* path)` — opens file, reads Record 0 header (~100 bytes), checks `encryptionType` field at PalmDOC offset 12
- Called by `FileBrowserActivity` during directory scan for `.azw`/`.azw3` entries; result drives greyed-out visual state
- Does not construct a `Mobi` instance; minimal heap impact during scan
- `loadHeader()` also checks `encryptionType == 2` and returns `false` with `LOG_ERR` — defence in depth

**Bounds Checking**
- `2×(1<<codeLength)` check on every CDIC dict lookup — enforced at iterative decode level
- All PalmDB record offset reads bounds-checked against `fileSize` before dereference
- CDIC record cap checked before any `malloc` — rejection occurs at count, not after allocation

**Error Routing**
- All rejection paths (`loadHeader()` DRM, CDIC cap, malformed header) return `false`
- `ReaderActivity::onEnter()` handles `Mobi::load()` → `false` by displaying `FullScreenMessageActivity`
- `Mobi` sets a `lastError` enum that `ReaderActivity` maps to the correct i18n key

---

### UI & Activity Layer

**DRM Visual Indicator**
- `FileBrowserActivity` calls `Mobi::isDrmLocked(path)` for `.azw`/`.azw3` entries during directory render
- Greyed-out entry rendered using existing disabled-item visual pattern in `FileBrowserActivity`
- No new activity or screen required

**Error Messages — New i18n Keys**
Two new `STR_*` keys added to all translation YAML files:
- `STR_DRM_PROTECTED` — displayed when DRM-locked file opened
- `STR_CDIC_CAP_EXCEEDED` — displayed when CDIC record count exceeds cap
- Both surfaced via `FullScreenMessageActivity`; i18n generator run after YAML update

**Chapter Navigation**
- Existing MOBI chapter selection UI pattern reused unchanged
- `parseToc()` populates the same chapter struct; chapter list activity requires no modifications

---

### Build, Cache & Versioning

**Cache Version Bump: First Commit**
- `VOFFSET_VERSION` bump to 2 is the first code change on the implementation branch
- Ensures no stale v1 cache can be misread as v2 during development
- MOBI7 cache rebuild is silent and automatic

**Extension Recognition**
- `hasMobiExtension()` in `lib/FsHelpers/FsHelpers.cpp` extended to include `.azw`, `.azw3`, `.prc`
- All 5 existing callers inherit the change with no modification: `FileBrowserActivity`, `ReaderActivity`, `RecentBooksStore`, `SleepActivity`, `Mobi.cpp` title fallback
- Phase 1 (extension only) ships as a standalone commit before Huffman work begins

**Build Validation**
- `pio run` (zero errors, zero `-Wall` warnings) required before any hardware test
- `pio check` (cppcheck) run after Huffman/CDIC implementation
- `ESP.getFreeHeap()` after `Mobi::load()` on KF8 file: > 150KB target; measured on device

### Decision Impact Analysis

**Implementation Sequence:**
1. `voffsets.bin` v2 version bump
2. `hasMobiExtension()` extension (`.azw`, `.azw3`, `.prc`) + title fallback fix
3. `Mobi::isDrmLocked()` static helper + `FileBrowserActivity` greyed-out indicator
4. New i18n keys (`STR_DRM_PROTECTED`, `STR_CDIC_CAP_EXCEEDED`) + YAML update + regenerate
5. `MobiVariant` enum + `detectFormat()` + `loadHeader()` DRM check
6. `HuffTable` + `CdicTable` structs + `loadHuffCdic()` + iterative `decompressHuffCdic()`
7. KF8 boundary parsing + `readContent()` KF8 dispatch
8. `parseToc()` INDX parsing + chapter navigation integration
9. Cache header v2 fields serialization/deserialization

**Cross-Component Dependencies:**
- `isDrmLocked()` depends on PalmDB record-offset parsing (shared with `loadHeader()`) — extract to a minimal common reader or duplicate the ~20-line header read
- `detectFormat()` must run before `loadHuffCdic()` — sequence enforced inside `load()`
- i18n keys must exist before `ReaderActivity` error routing can reference them
- `voffsets.bin` v2 must land before any KF8 cache writes touch SD card

## Implementation Patterns & Consistency Rules

### Naming Patterns

**New Mobi class methods — camelCase, verb-first:**
```cpp
bool detectFormat();          // ✓
bool loadHuffCdic();          // ✓
bool decompressHuffCdic(...); // ✓
bool parseToc();              // ✓
static bool isDrmLocked(const char* path); // ✓
```

**Private member variables — camelCase, no prefix:**
```cpp
MobiVariant mobiVariant;      // ✓
uint32_t kf8SectionRecord;    // ✓
HuffTable huffTable;          // ✓
CdicTable cdicTable;          // ✓
// NOT: _mobiVariant, m_mobiVariant, mobi_variant
```

**New constants — `static constexpr`, UPPER_SNAKE_CASE:**
```cpp
static constexpr uint8_t MAX_CDIC_RECORDS = 10;
static constexpr uint8_t VOFFSET_VERSION = 2;
static constexpr uint16_t COMPRESSION_HUFFMAN = 17480;
// NOT: #define, const int, magic literals
```

**LOG_ERR/LOG_DBG module tag: always `"MOBI"`**
```cpp
LOG_ERR("MOBI", "DRM-encrypted file: %s", path);
LOG_DBG("MOBI", "KF8 section at record %d", kf8SectionRecord);
// NOT: "KF8", "AZW", "HUFF" — single tag for the whole Mobi subsystem
```

---

### Structure Patterns

**New struct declarations: private inner structs in `Mobi.h`**
```cpp
// Mobi.h — inside class Mobi, private section:
struct HuffTable {
    uint32_t dict1[256];
    uint32_t dict2[64];
};
struct CdicTable {
    uint8_t* records[MAX_CDIC_RECORDS];
    uint8_t  recordCount;
    uint16_t phrasesPerRecord;
};
// NOT: separate HuffTable.h, not top-level structs
```

**`isDrmLocked()` implementation: in `Mobi.cpp`, not a new file**
- No new `.cpp` files for this feature; all new code lives in `Mobi.cpp` and `Mobi.h`
- `FsHelpers.cpp` changes are the only other file touched (extension recognition)

---

### Memory Patterns

**malloc: always check, always set to nullptr after free**
```cpp
auto* buf = static_cast<uint8_t*>(malloc(size));
if (!buf) {
    LOG_ERR("MOBI", "malloc failed: %d bytes", size);
    return false;
}
// ... use buf ...
free(buf);
buf = nullptr;
```

**CDIC block allocation — cap check before any malloc:**
```cpp
if (cdicCount > MAX_CDIC_RECORDS) {
    LOG_ERR("MOBI", "CDIC record count %d exceeds cap", cdicCount);
    return false;  // No allocations yet; clean exit
}
for (uint8_t i = 0; i < cdicCount; i++) {
    cdicTable.records[i] = static_cast<uint8_t*>(malloc(CDIC_RECORD_SIZE));
    if (!cdicTable.records[i]) { /* free all allocated so far, return false */ }
}
```

**CDIC block free — destructor iterates to recordCount only:**
```cpp
~Mobi() {
    for (uint8_t i = 0; i < cdicTable.recordCount; i++) {
        free(cdicTable.records[i]);
        cdicTable.records[i] = nullptr;
    }
}
```

---

### Binary Serialization Patterns

**Always use `memcpy` for multi-byte reads from file buffers (RISC-V alignment):**
```cpp
// CORRECT:
uint32_t val;
memcpy(&val, buf + offset, sizeof(val));
val = __builtin_bswap32(val);  // big-endian conversion

// WRONG — may fault on unaligned address:
uint32_t val = *reinterpret_cast<const uint32_t*>(buf + offset);
```

**Cache header: write field-by-field, not struct memcpy:**
- `VOffsetCacheHeader` is written one field at a time via `FsFile::write()`
- Avoids packing/alignment divergence between struct in memory and bytes on disk
- Each field explicitly byte-swapped if needed (all cache values stored little-endian)

---

### Error Handling Patterns

**`lastError` enum: member of `Mobi`, set before returning false**
```cpp
enum class MobiError : uint8_t { None=0, DrmProtected=1, CdicCapExceeded=2, MalformedHeader=3 };
MobiError lastError = MobiError::None;

// Usage:
lastError = MobiError::DrmProtected;
LOG_ERR("MOBI", "DRM-encrypted file");
return false;
```

**`ReaderActivity` maps `lastError` → i18n key:**
```cpp
if (!mobi->load()) {
    StrId msgKey = STR_UNKNOWN_ERROR;
    switch (mobi->getLastError()) {
        case MobiError::DrmProtected:    msgKey = STR_DRM_PROTECTED; break;
        case MobiError::CdicCapExceeded: msgKey = STR_CDIC_CAP_EXCEEDED; break;
        default: break;
    }
    // show FullScreenMessageActivity with tr(msgKey)
}
```

**All error paths: LOG_ERR then return false — never abort(), never silent return**

---

### Process Patterns

**Bounds checking: validate before every PalmDB record index access**
```cpp
if (recordIdx >= recordCount) {
    LOG_ERR("MOBI", "Record index %d out of bounds (%d)", recordIdx, recordCount);
    return false;
}
uint32_t offset = recordFileOffsets[recordIdx];
if (offset >= fileSize) {
    LOG_ERR("MOBI", "Record offset 0x%x beyond file size 0x%x", offset, fileSize);
    return false;
}
```

**File I/O: always via `Storage.openFileForRead()` — never raw SdFat directly**
```cpp
FsFile file;
if (!Storage.openFileForRead("MOBI", path, file)) return false;
// ... read ...
file.close();
```

**`isDrmLocked()` — minimal read, always close file:**
```cpp
static bool isDrmLocked(const char* path) {
    FsFile file;
    if (!Storage.openFileForRead("MOBI", path, file)) return false;
    uint8_t buf[100];
    bool ok = (file.read(buf, sizeof(buf)) == sizeof(buf));
    file.close();
    if (!ok) return false;
    // parse PalmDB record 0 offset, read encryptionType
    ...
}
```

---

### Enforcement Guidelines

**All agents implementing this feature MUST:**
- Use `"MOBI"` as the LOG tag for all new code in `Mobi.cpp`
- Check malloc result before use; set pointer to `nullptr` after free
- Use `memcpy` for all multi-byte reads from byte buffers — never pointer cast
- Check CDIC record cap before any allocation in `loadHuffCdic()`
- Write cache header fields individually, not as a struct dump
- Set `lastError` before every `return false` path in `Mobi::load()`
- Declare new constants as `static constexpr`, not `#define`

**Anti-patterns to avoid:**
- `*reinterpret_cast<uint32_t*>(buf + offset)` — RISC-V alignment fault
- `malloc` without nullptr check
- `LOG_ERR` without a subsequent `return false`
- Adding new public methods to `Mobi` — the `readContent()` contract is the only public interface that matters
- Raw `SdFat` calls bypassing `HalStorage`

## Project Structure & Boundaries

### Feature File Scope

This is a brownfield extension. No new directories are created. The feature touches exactly these files:

**Modified files:**
```
lib/FsHelpers/FsHelpers.cpp          ← hasMobiExtension() + .azw/.azw3/.prc
lib/FsHelpers/FsHelpers.h            ← declaration update
lib/Mobi/Mobi.h                      ← new structs, enum, member declarations
lib/Mobi/Mobi.cpp                    ← all new implementation (~500 LOC)
lib/I18n/translations/english.yaml  ← STR_DRM_PROTECTED, STR_CDIC_CAP_EXCEEDED (reference)
lib/I18n/translations/*.yaml        ← same keys in all 20 language files
src/activities/home/FileBrowserActivity.cpp  ← isDrmLocked() call + greyed-out render
src/activities/reader/ReaderActivity.cpp     ← getLastError() → FullScreenMessageActivity
```

**New files (if chapter nav requires its own activity):**
```
src/activities/reader/MobiReaderChapterSelectionActivity.cpp  ← TBD: verify if needed
src/activities/reader/MobiReaderChapterSelectionActivity.h    ← TBD: verify if needed
```
> **Verify before implementation:** Check whether `MobiReaderActivity` already has chapter navigation or if it defers to a shared pattern. If a new activity is needed, it follows the `EpubReaderChapterSelectionActivity` pattern.

**Generated (never committed):**
```
lib/I18n/I18nKeys.h        ← regenerated: python scripts/gen_i18n.py ...
lib/I18n/I18nStrings.h     ← regenerated
lib/I18n/I18nStrings.cpp   ← regenerated
```

**Zero-change guarantee — these files MUST NOT be modified:**
```
src/activities/reader/MobiReaderActivity.cpp/.h   ← format-agnostic; unchanged
src/activities/reader/ReaderActivity.h            ← routing unchanged
src/activities/reader/TxtReaderActivity.cpp/.h    ← unchanged
src/main.cpp                                      ← unchanged
lib/GfxRenderer/                                  ← unchanged
lib/Epub/                                         ← unchanged
```

---

### SD Card Cache Structure

```
SD:/
└── .crosspoint/
    └── mobi_<hash>/          ← hash of file path (existing pattern)
        ├── voffsets.bin      ← v2 format (bumped from v1)
        └── progress.bin      ← unchanged (virtual byte offset)
```

`voffsets.bin` v2 header layout (field-by-field, little-endian):
```
[0]  uint32_t magic           = 0x4D424F49
[4]  uint8_t  version         = 2
[5]  uint32_t fileSize
[9]  uint16_t textRecordCount
[11] uint16_t compressionType
[13] uint16_t extraDataFlags
[15] uint8_t  mobiVariant     = 0 (MOBI7) | 1 (KF8)   ← new
[16] uint32_t kf8SectionRecord                          ← new (0xFFFFFFFF if MOBI7)
[20] uint8_t  cdicCount                                 ← new
[21] uint32_t virtualOffsets[]
```

---

### Architectural Boundaries

**`Mobi` class boundary (lib/Mobi/):**
- Public interface (callers may use): `load()`, `readContent()`, `getLastError()`, `isDrmLocked()` (static)
- All KF8/Huffman/CDIC complexity is private; no caller sees `HuffTable`, `CdicTable`, `MobiVariant`
- `MobiReaderActivity` uses only `readContent()` — format-transparent

**`FileBrowserActivity` boundary:**
- Calls `Mobi::isDrmLocked(path)` per `.azw`/`.azw3` entry during directory render
- No other Mobi coupling; does not construct a `Mobi` instance
- Greyed-out render uses existing disabled-item visual pattern (no new UI component)

**`ReaderActivity` boundary:**
- Calls `mobi->load()` and checks return; calls `mobi->getLastError()` on failure
- Maps `MobiError` enum → `StrId` → `FullScreenMessageActivity`
- Does not know about Huffman, CDIC, or KF8 internals

**i18n boundary:**
- New keys defined in YAML source files
- Generated headers (`I18nKeys.h`, `I18nStrings.h/cpp`) are the only i18n interface used in C++
- `tr(STR_DRM_PROTECTED)` and `tr(STR_CDIC_CAP_EXCEEDED)` are the only call sites

---

### Requirements → Files Mapping

| FR Category | Files |
|---|---|
| FR1–3 File Discovery | `FsHelpers.cpp/.h`, `FileBrowserActivity.cpp` |
| FR4–7 Format Detection | `Mobi.cpp/.h` (`detectFormat`, `loadHeader`) |
| FR8–11 Decompression | `Mobi.cpp/.h` (`loadHuffCdic`, `decompressHuffCdic`) |
| FR12–16 Virtual Access | `Mobi.cpp/.h` (`readContent`, `buildVirtualOffsetTable`, `voffsets.bin` v2) |
| FR17–19 Chapter Nav | `Mobi.cpp/.h` (`parseToc`), possibly new `MobiReaderChapterSelectionActivity` |
| FR20–22 Reading/Progress | `MobiReaderActivity.cpp` (zero changes), `Mobi.cpp` (progress byte offset unchanged) |
| FR23–26 Error Handling | `Mobi.cpp` (`lastError`), `ReaderActivity.cpp`, `translations/*.yaml` |

---

### Data Flow

```
FileBrowserActivity (directory scan)
    └── Mobi::isDrmLocked(path) ──→ bool ──→ greyed render or normal render

ReaderActivity::onEnter()
    └── Mobi::load()
         ├── detectFormat()       reads MOBI type + EXTH 121 → mobiVariant
         ├── loadHeader()         DRM check → lastError = DrmProtected → false
         ├── loadHuffCdic()       malloc HUFF+CDIC blocks (KF8 only)
         ├── buildVirtualOffsets() iterates records → decompresses → counts bytes
         ├── parseToc()           INDX records → chapter struct (KF8 only)
         └── saveCache()          writes voffsets.bin v2 to SD
    ├── success → MobiReaderActivity (readContent() calls only)
    └── failure → getLastError() → StrId → FullScreenMessageActivity

MobiReaderActivity::loop()
    └── Mobi::readContent(offset, length)
         ├── mobiVariant == MOBI7 → existing PalmDOC path
         └── mobiVariant == KF8  → decompressHuffCdic() → stripHtml()
```

## Architecture Validation Results

### Coherence Validation ✅

**Decision Compatibility:**
All decisions form a coherent chain: `detectFormat()` establishes `mobiVariant` before `loadHuffCdic()` is called; the iterative decoder uses a fixed 128-byte work stack rather than recursion, eliminating the FreeRTOS overflow risk; CDIC array-of-blocks avoids a single large contiguous allocation that fragmented heaps cannot satisfy; `readContent()` dispatches on `mobiVariant` — zero change required above that boundary.

**Pattern Consistency:**
All new methods follow camelCase verb-first naming; all constants use `static constexpr` UPPER_SNAKE_CASE; all LOG calls use `"MOBI"` tag; all multi-byte buffer reads use `memcpy`; all malloc sites check for nullptr and free on failure paths. No contradictions with existing `Mobi.cpp` patterns.

**Structure Alignment:**
No new directories needed. All new code is in `lib/Mobi/Mobi.cpp/.h` with two small touch points (`FsHelpers`, `FileBrowserActivity`, `ReaderActivity`). Component boundaries are strict — no caller above `Mobi` sees format internals.

---

### Requirements Coverage Validation

**Functional Requirements (26/26 covered):**

| FR | Coverage | Notes |
|---|---|---|
| FR1–3 | ✅ | `hasMobiExtension()` + `isDrmLocked()` |
| FR4–7 | ✅ | `loadHeader()` + `detectFormat()` |
| FR8–11 | ✅ | PalmDOC unchanged; Huffman/CDIC new; cap enforced |
| FR12–16 | ✅ | `readContent()` contract preserved; voffsets.bin v2 |
| FR17–19 | ✅* | `parseToc()` implemented; chapter UI TBD (see gap below) |
| FR20–22 | ✅ | `MobiReaderActivity` unchanged; progress.bin preserved |
| FR23–26 | ✅ | `lastError` enum; `STR_DRM_PROTECTED`; `STR_CDIC_CAP_EXCEEDED`; bounds-checked PalmDB reads |

**Non-Functional Requirements (11/11 covered):**

| NFR | Coverage |
|---|---|
| NFR1 Page-turn ≤ 100ms | ✅ Record-streaming; decompressor per-record not per-page |
| NFR2 First-open ≤ 5s | ✅ CDIC loaded once at `load()`; virtual offset table built once and cached |
| NFR3 Subsequent opens fast | ✅ voffsets.bin cache eliminates rebuild |
| NFR4 Heap > 150KB after load | ✅ CDIC max 40KB + working buffers ~20KB; fits in 380KB budget |
| NFR5 Stack > 512B headroom | ✅ Iterative decoder; 128-byte explicit stack replaces recursion |
| NFR6 No heap fragmentation | ✅ CDIC held for session; working bufs malloc/free per readContent call |
| NFR7 CDIC cap 40KB hard limit | ✅ Cap check before any malloc |
| NFR8 Zero crashes | ✅ All error paths return false; no abort() |
| NFR9 LOG_ERR + false on errors | ✅ Enforced by pattern rules |
| NFR10 Clean state on rejection | ✅ Destructor frees CDIC blocks; FullScreenMessageActivity → back to browser |
| NFR11 Zero compiler warnings | ✅ Build validation required before hardware test |

---

### Gap Analysis

**Important Gap — Chapter Nav UI (FR18/FR19):**
`parseToc()` populates the existing chapter struct type. Whether `MobiReaderActivity` already has a chapter list trigger, or a new `MobiReaderChapterSelectionActivity` is required, must be verified before implementing story 8.
- **Action**: Before implementing `parseToc()`, read `MobiReaderActivity.cpp` and `EpubReaderChapterSelectionActivity.cpp` to determine the correct integration pattern.
- **If new activity needed**: follow `EpubReaderChapterSelectionActivity` pattern exactly.
- **Chapter nav can be deferred to Growth if unexpectedly complex** (PRD §"Resource Risks").

**Minor Risk — Progress Offset on Cache Rebuild:**
`progress.bin` stores a virtual byte offset. When `voffsets.bin` rebuilds due to the v1→v2 version bump, stale progress offsets may not map correctly to the rebuilt table.
- **Mitigation**: Reset `progress.bin` to 0 when a cache rebuild is triggered.
- **Document**: Add note to `docs/file-formats.md` under voffsets.bin v2.

---

### Architecture Completeness Checklist

**✅ Requirements Analysis**
- [x] Project context thoroughly analyzed
- [x] Scale and complexity assessed (medium, embedded IoT)
- [x] Technical constraints identified (380KB RAM, FreeRTOS stack, RISC-V alignment)
- [x] Cross-cutting concerns mapped (memory, error handling, cache, format detection)

**✅ Architectural Decisions**
- [x] Critical decisions documented (iterative decoder, CDIC cap, version bump, detectFormat)
- [x] Technology stack specified (C++20, PlatformIO, FreeRTOS, SdFat)
- [x] Integration patterns defined (Mobi class boundary, error routing, i18n)
- [x] Performance and memory constraints addressed

**✅ Implementation Patterns**
- [x] Naming conventions established (camelCase, static constexpr, "MOBI" log tag)
- [x] Memory patterns defined (malloc/check/free/nullptr, array-of-blocks)
- [x] Binary serialization patterns (memcpy, field-by-field cache write)
- [x] Error handling patterns (lastError enum, LOG_ERR + return false)
- [x] Anti-patterns documented (reinterpret_cast, recursive decoder, raw SdFat)

**✅ Project Structure**
- [x] Exact file scope defined (8 modified + 2 TBD new + 3 generated)
- [x] Zero-change file list documented
- [x] Component boundaries established
- [x] SD cache format v2 header layout specified
- [x] FR → file mapping complete

---

### Architecture Readiness Assessment

**Overall Status: READY FOR IMPLEMENTATION**

**Confidence Level: High**

**Key Strengths:**
- Tight scope: all new complexity absorbed inside `Mobi` class; zero changes to reader activities
- Memory budget well-understood: CDIC max 40KB + buffers; verified against 380KB ceiling
- Known CVE mitigated by design (iterative decoder, `2×(1<<codeLength)`)
- Implementation sequence sequenced to prevent cache corruption during development
- Chapter nav deferral path identified if INDX parsing proves complex

**Areas for Future Enhancement:**
- Cover art extraction from KF8 image records (Growth phase)
- KFX/AZW8 support (Vision phase; blocked on spec)
- Rich text / CSS rendering (out of scope for this fork)

---

### Implementation Handoff

**First implementation step:** Bump `VOFFSET_VERSION` to 2 in `Mobi.cpp` — before any other change.

**Implementation sequence** (from Core Architectural Decisions):
1. `VOFFSET_VERSION` bump → 2
2. `hasMobiExtension()` extension + title fallback
3. `Mobi::isDrmLocked()` + `FileBrowserActivity` greyed-out
4. New i18n keys + YAML update + regenerate
5. `MobiVariant` enum + `detectFormat()` + DRM check in `loadHeader()`
6. `HuffTable` + `CdicTable` + `loadHuffCdic()` + iterative `decompressHuffCdic()`
7. KF8 boundary parsing + `readContent()` KF8 dispatch
8. `parseToc()` INDX parsing + chapter nav (verify activity pattern first)
9. Cache header v2 serialization/deserialization

**Build gate:** `pio run` must produce zero errors and zero `-Wall` warnings before any hardware testing.
