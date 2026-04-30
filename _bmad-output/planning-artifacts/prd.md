---
stepsCompleted: [step-01-init, step-02-discovery, step-02b-vision, step-02c-executive-summary, step-03-success, step-04-journeys, step-05-domain, step-06-innovation, step-07-project-type, step-08-scoping, step-09-functional, step-10-nonfunctional, step-11-polish]
inputDocuments:
  - _bmad-output/planning-artifacts/research/technical-azw-azw3-mobi-support-research-2026-04-27.md
  - docs/fork-decisions.md
  - docs/file-formats.md
workflowType: 'prd'
classification:
  projectType: iot_embedded
  domain: general
  complexity: medium
  projectContext: brownfield
---

# Product Requirements Document - crosspoint-reader-mods

**Author:** Nick
**Date:** 2026-04-27

## Executive Summary

The CrossPoint Reader is an open-source e-ink firmware for the Xteink X4 (ESP32-C3), targeting users who expect a capable reading device without platform lock-in. This feature extends the existing MOBI reader subsystem to support the full Amazon Kindle format family: `.azw` (Kindle Format 7), `.azw3` (Kindle Format 8 / KF8), and `.prc` file extensions. Maximum format compatibility is a baseline expectation for a device that positions itself as a Kindle rival — users should be able to sideload files from their existing Amazon library or Calibre exports without a conversion step.

The feature operates within strict hardware constraints: 380KB usable DRAM, no PSRAM, single-core RISC-V at 160MHz (ESP32-C3, PlatformIO, C++20). All format complexity is absorbed inside the existing `Mobi` class; `MobiReaderActivity` and the rest of the reader pipeline require no changes. DRM-encrypted files (encryption type 2) are detected at parse time and rejected with a clear user-facing error — both in the reader and as a visual indicator in the file browser/library screen. Chapter navigation (TOC extraction from KF8 index records) is in-scope for MVP.

The CrossPoint reader directly reads the Kindle's own native binary formats — no Calibre conversion, no format-wrangling. The record-streaming architecture (decompressing one 4096-byte chunk at a time) makes this feasible on 380KB RAM where full-load approaches used by reference implementations (libmobi, KindleUnpack) would OOM. The Huffman/CDIC decompressor is implemented iteratively with an explicit work stack, avoiding FreeRTOS stack overflow — a known failure mode in at least one open-source MOBI decompressor (CVE-2026-25920, SumatraPDF).

## Success Criteria

### User Success

- A user with a DRM-free `.azw` or `.azw3` file on their SD card opens it directly from the file browser — no Calibre conversion step required
- Chapter navigation works identically to the existing MOBI reader: user can jump to any chapter from the in-reader menu
- A DRM-locked `.azw` or `.azw3` file is visually distinguished in the file browser (greyed-out entry) and produces a clear, non-crashing error message when opened
- Text rendering quality is indistinguishable from existing MOBI7 output for the same content

### Technical Success

- `ESP.getFreeHeap()` after `Mobi::load()` on a KF8 file: **> 150KB** remaining
- `uxTaskGetStackHighWaterMark()` on the reader task during AZW3 reading: **> 512 bytes** headroom
- Page-turn latency: unchanged vs existing MOBI7 path (target < 100ms SD-to-render)
- No heap fragmentation accumulation across 20+ page turns (largest free block stable)
- CDIC table allocation capped at 10 records maximum; files exceeding this are rejected gracefully with `LOG_ERR` + user message
- DRM detection fires at `loadHeader()` — no decompression attempted on encrypted content
- `VOFFSET_VERSION` bumped to 2; existing MOBI7 caches silently rebuild on first open

### Measurable Outcomes

- All DRM-free test files from Project Gutenberg (MOBI) and Standard Ebooks (AZW3) open and render correctly
- Text output matches KindleUnpack reference extraction (whitespace-normalised diff)
- Zero crashes or watchdog resets during a full book read session on device

## Product Scope

### MVP — Minimum Viable Product

- `.azw`, `.azw3`, `.prc` extension recognition in `FsHelpers` and all callers
- DRM detection (encryption type 2) at `loadHeader()`: greyed-out file browser entry + `FullScreenMessageActivity` error on open attempt
- Huffman/CDIC decompressor — iterative implementation, explicit work stack, correct `2×(1<<codeLength)` bounds check
- KF8 boundary detection via EXTH record 121; KF8 section header parsing
- KF8 text record decompression via Huffman/CDIC → `stripHtml()` → virtual flat text (same pipeline as MOBI7)
- Chapter navigation: TOC extracted from KF8 INDX records, surfaced via the existing MOBI chapter selection UI pattern
- SD cache: `voffsets.bin` v2 format with KF8 metadata fields; MOBI7 caches auto-invalidate and rebuild

### Growth Features (Post-MVP)

- Cover art from KF8 embedded image records (currently falls back to sidecar file; Growth adds extraction from within the `.azw3` file itself)
- KFX / AZW8 support (blocked on community reverse-engineering of closed spec; add when a viable open-source parser exists)

### Vision (Future)

- Rich text rendering for KF8: CSS styling, embedded font rendering — requires a significant architecture change to the plain-text pipeline and is not planned for this fork in the foreseeable future

## User Journeys

### Journey 1 — Alex: Sideloading a DRM-free AZW3 from Calibre (Primary Success Path)

**User profile:** Kindle owner who has converted personal library to DRM-free AZW3 via DeDRM plugin + Calibre. Copies `.azw3` file to SD card.

**Journey:**
1. Alex copies `the-name-of-the-wind.azw3` to the SD card root
2. Opens file browser on CrossPoint Reader — file appears normally (not greyed)
3. Selects the file; device shows "Loading…" briefly
4. `Mobi::load()` runs: PalmDB header parsed → EXTH record 121 found → KF8 boundary located → Huffman/CDIC tables loaded into DRAM → virtual offset table built and cached to `voffsets.bin`
5. Reader opens at last reading position (or page 1 on first open)
6. Alex navigates to chapter list — chapters drawn from KF8 INDX records, identical UX to existing MOBI reader
7. Page turns are smooth; decompression of one 4096-byte chunk per turn; heap stays > 150KB throughout
8. Alex exits reader; progress saved; next open resumes from same position (virtual offset table loaded from cache — no rebuild)

**Key capabilities exercised:** extension detection, Huffman/CDIC decompressor, KF8 section parsing, virtual offset table, chapter navigation, progress persistence, cache load

---

### Journey 2 — Sam: Opening a DRM-locked AZW File (Graceful Failure Path)

**User profile:** Has an Amazon-purchased `.azw` still protected by DRM (encryption type 2).

**Journey:**
1. Sam copies `purchased-book.azw` to the SD card
2. Opens file browser — file appears **greyed out** with a lock indicator; Sam can still select it
3. Sam selects the file
4. `Mobi::loadHeader()` detects `encryptionType == 2` at parse time; no decompression is attempted
5. `FullScreenMessageActivity` displays a clear error: e.g., *"This file is DRM-protected and cannot be opened."*
6. Sam presses Back — returns to file browser cleanly; device state unchanged

**Key capabilities exercised:** DRM detection at `loadHeader()`, greyed-out file browser entry, `FullScreenMessageActivity` error, no crash, no partial state

---

### Journey 3 — Jordan: Opening an Oversized KF8 File (Cap Enforcement Path)

**User profile:** Has an unusually large AZW3 with more than 10 CDIC records (edge case from obscure publisher toolchain).

**Journey:**
1. Jordan copies the file to the SD card; it appears normally in the file browser (DRM check passes)
2. Selects the file; `Mobi::load()` begins KF8 parsing
3. CDIC record count exceeds the cap of 10 — `load()` logs `LOG_ERR` and returns false
4. `FullScreenMessageActivity` displays a clear error message
5. Jordan returns to the file browser; device stable

**Key capabilities exercised:** CDIC record cap enforcement, graceful rejection, no heap exhaustion, clean error path

---

### Journey 4 — Developer: Debugging and Extending the MOBI Subsystem

**User profile:** Developer adding a test case or diagnosing a decompression discrepancy against KindleUnpack output.

**Journey:**
1. Developer connects device via USB, opens serial monitor (`python3 scripts/debugging_monitor.py`)
2. Opens a test AZW3 file; `LOG_DBG` output shows: PalmDB record count, KF8 boundary offset, CDIC record sizes, heap before/after `load()`, `uxTaskGetStackHighWaterMark()` reading
3. Developer exports same file via KindleUnpack (`kindleunpack.py book.azw3 out/`), extracts raw text, runs whitespace-normalised diff against `readContent()` output
4. If discrepancy found: adjusts `stripHtml()` or Huffman decode logic, rebuilds with `pio run`, reflashes, retests
5. Cache on SD is regenerated automatically on next open (version bump detects stale `voffsets.bin`)

**Key capabilities exercised:** serial logging, heap/stack diagnostics, KindleUnpack reference comparison, cache invalidation on version bump

---

### Journey Requirements Summary

| Capability | J1 (DRM-free AZW3) | J2 (DRM-locked AZW) | J3 (Oversized KF8) | J4 (Developer) |
|---|---|---|---|---|
| `.azw` / `.azw3` / `.prc` extension recognition | ✓ | ✓ | ✓ | ✓ |
| DRM detection + greyed file browser entry | — | ✓ | — | — |
| `FullScreenMessageActivity` error on open | — | ✓ | ✓ | — |
| Huffman/CDIC decompressor (iterative) | ✓ | — | ✓ (cap triggered) | ✓ |
| KF8 boundary + section parsing | ✓ | — | ✓ | ✓ |
| Virtual offset table (v2 cache) | ✓ | — | — | ✓ |
| Chapter navigation from INDX records | ✓ | — | — | ✓ |
| CDIC record cap (10 max) | — | — | ✓ | ✓ |
| Progress persistence | ✓ | — | — | — |
| Serial diagnostics / heap reporting | — | — | — | ✓ |

## IoT/Embedded Specific Requirements

### Hardware Requirements

- **MCU**: ESP32-C3, single-core RISC-V @ 160MHz
- **DRAM budget**: 380KB usable; no PSRAM — hard ceiling, not a soft target
- **Flash**: 16MB; constant data (`constexpr` lookup tables, font data) must reside in flash
- **Storage I/O**: SD card via SPI, accessed through `HalStorage` / SdFat `FsFile` API; no POSIX, no `fopen`
- **Display**: 800×480 e-ink; framebuffer = 48KB (single-buffer mode); page turn target < 100ms SD-to-render

### Performance Profile

- Decompression speed: as fast as the hardware allows; no artificial latency budget
- CDIC table load (at `load()`) is a one-time cost per session; acceptable up to ~1–2s on first open
- Per-record Huffman decode must complete within the existing page-turn latency envelope (< 100ms)
- Battery/power draw: not a design constraint for this feature; assessed via real-world testing post-implementation

### Security Model

- **Threat model**: personal-use device reading user-supplied local SD card files; no network attack surface
- **DRM**: reject at `loadHeader()` (encryption type 2); no decompression attempted on encrypted content
- **Defensive parsing**: all PalmDB record offset reads bounds-checked against `fileSize`; malformed headers return `false` with `LOG_ERR`, never read past buffer end
- **No path traversal risk**: file paths sourced exclusively from SD card directory listing, not user-typed input

### Update Mechanism

Not applicable to this feature; firmware OTA is handled separately by the existing OTA subsystem.

## Innovation & Novel Patterns

### Detected Innovation Areas

**1. Record-streaming Huffman/CDIC decompression on 380KB RAM**

All existing open-source KF8 implementations (libmobi, KindleUnpack, Calibre) use a full-load model: load all text records into DRAM, then decompress. On ESP32-C3 with 380KB usable DRAM and no PSRAM, this approach immediately OOMs. This project implements the first record-streaming KF8 decompressor: only one 4096-byte compressed chunk and its decompressed output exist in RAM at any time. CDIC lookup tables (~12–40KB total across ≤10 records) are loaded once at `load()` and held for the session. This is the only architecture that makes KF8 reading feasible on this hardware class.

**2. Iterative Huffman decoder with explicit work stack**

Recursive Huffman/CDIC decompression is the established pattern in every reference implementation. On FreeRTOS with a 2048-byte task stack, deep recursion causes silent stack overflow crashes. This project replaces recursion with an explicit iteration loop and a bounded work stack (~128 bytes), eliminating the overflow failure mode. This directly addresses CVE-2026-25920 (SumatraPDF, 2026) — a wrong bounds check in a recursive HuffDic implementation — by using a fundamentally different approach where the correct `2×(1<<codeLength)` bound is enforced at the iterative level.

**3. Format-transparent virtual flat-text interface**

The `Mobi::readContent(offset, length)` interface presents the decompressed, HTML-stripped text stream as a single virtual flat file regardless of whether the source is PalmDOC-compressed MOBI7, uncompressed MOBI7, or Huffman/CDIC-compressed KF8. `MobiReaderActivity` and the entire reader pipeline above it require zero changes to support a format with a radically different compression algorithm. New format complexity is absorbed entirely at the `Mobi` class boundary.

### Market Context & Competitive Landscape

No other open-source ESP32 e-reader firmware (OpenEPaperLink, lilygo-epd, OpenBook) reads KF8/AZW3 natively. The Kindle ecosystem lock-in typically forces users through Calibre conversion. This project eliminates that step for DRM-free content, which is the primary use case for sideloaded Kindle libraries.

### Risk Mitigation

| Risk | Mitigation |
|---|---|
| Huffman table corruption → decode garbage | Bounds check on every dict lookup; `LOG_ERR` + reject on out-of-range |
| CDIC table exhausts heap | Hard cap at 10 records; files exceeding cap rejected before any CDIC allocation |
| Recursive decode path reaches stack limit | Iterative decoder eliminates recursion entirely |
| KF8 boundary detection false positive | Validate KF8 section header magic bytes after EXTH 121 pointer before treating as KF8 |

## Project Scoping & Phased Development

### MVP Strategy & Philosophy

**MVP Approach:** Experience MVP — the minimum that makes DRM-free Kindle sideloads work end-to-end, indistinguishable from MOBI reading quality. No partial states, no "almost works."

**Resource requirements:** Single developer (Nick); no external dependencies; no new libraries.

### Post-MVP Features

**Phase 2 (Growth):**
- Cover art extracted from KF8 embedded image records (currently falls back to sidecar file)
- KFX / AZW8 support — blocked on community reverse-engineering; add when viable open-source parser exists

**Phase 3 (Vision):**
- Rich text rendering for KF8 (CSS styling, embedded fonts) — requires architecture change to plain-text pipeline; out of scope for this fork in the foreseeable future

### Risk Mitigation Strategy

**Technical risks:**
- Huffman/CDIC correctness: validate against KindleUnpack reference extraction (whitespace-normalised diff) on every test file before declaring done
- RAM budget: measure `ESP.getFreeHeap()` after `load()` throughout development; if > 150KB headroom not maintained, CDIC record cap is the first lever to tighten

**Resource risks:**
- Single developer: scope cap is already lean; INDX/TOC parsing is the most complex sub-task — if it proves unexpectedly complex, chapter nav can be deferred to Growth without blocking basic reading

## Functional Requirements

### File Discovery & Recognition

- **FR1**: The file browser can display `.azw`, `.azw3`, and `.prc` files alongside `.mobi` files
- **FR2**: The file browser can visually distinguish DRM-protected Kindle files from openable files
- **FR3**: The system can identify DRM protection status during fast metadata scans (header-only load, no decompression)

### Format Detection & Parsing

- **FR4**: The system can parse PalmDB container headers for all Kindle format variants (AZW, AZW3, PRC)
- **FR5**: The system can detect KF8 content within a compound AZW3 file via the EXTH boundary pointer
- **FR6**: The system can parse KF8 section headers to locate text records
- **FR7**: The system can extract book title and author metadata from EXTH records

### Content Decompression

- **FR8**: The system can decompress PalmDOC-compressed (type 2) text records from Kindle files
- **FR9**: The system can decompress Huffman/CDIC-compressed (type 17480) text records from KF8 files
- **FR10**: The system can load and apply CDIC lookup tables required for Huffman decompression
- **FR11**: The system can enforce a maximum CDIC record count to prevent heap exhaustion

### Text Extraction & Virtual Access

- **FR12**: The system can strip HTML markup from KF8 text records to produce plain text
- **FR13**: The system can present Kindle content as a virtual flat text stream accessible by byte offset
- **FR14**: The system can build a virtual offset table mapping record boundaries to cumulative text byte positions
- **FR15**: The system can cache the virtual offset table to SD card for fast subsequent opens
- **FR16**: The system can detect and invalidate a stale virtual offset table, triggering a rebuild

### Chapter Navigation

- **FR17**: The system can extract a chapter table of contents from KF8 INDX records
- **FR18**: The reader can display the chapter list to the user
- **FR19**: The reader can navigate directly to a selected chapter by byte offset

### Reading & Progress

- **FR20**: The reader can paginate Kindle content using the existing layout and rendering pipeline
- **FR21**: The reader can save reading progress (virtual byte offset) to SD card
- **FR22**: The reader can restore reading progress on subsequent opens of the same file

### Error Handling & Rejection

- **FR23**: The system can detect DRM encryption at header parse time without attempting decompression
- **FR24**: The system can display a clear error message when a DRM-protected file is opened
- **FR25**: The system can display a clear error message when a file exceeds the CDIC record cap
- **FR26**: The system can safely reject malformed PalmDB files without crashing or corrupting state

## Non-Functional Requirements

### Performance

- **NFR1**: Page-turn latency (SD read → screen render complete) ≤ 100ms for KF8 files under normal SD card conditions
- **NFR2**: First-open load time (header parse + CDIC table load + virtual offset table build) ≤ 5 seconds for files up to 500 text records
- **NFR3**: Subsequent opens (virtual offset table loaded from SD cache) have latency indistinguishable from existing MOBI7 opens

### Memory Safety

- **NFR4**: `ESP.getFreeHeap()` after `Mobi::load()` on a KF8 file: > 150KB remaining
- **NFR5**: Reader task `uxTaskGetStackHighWaterMark()` during an AZW3 reading session: > 512 bytes headroom
- **NFR6**: No heap fragmentation accumulation across 20+ consecutive page turns (largest free block stable within ±10%)
- **NFR7**: CDIC table allocation hard-capped at 10 records (~40KB maximum); rejection occurs before any CDIC memory is allocated

### Reliability

- **NFR8**: Zero crashes (panic, abort, watchdog reset) during a complete book read session on device
- **NFR9**: All error paths return `false` with `LOG_ERR`; no `abort()`, no undefined behavior on malformed input
- **NFR10**: A DRM-protected or rejected file open leaves the device in a clean state — returns to file browser with no partial or corrupted state
- **NFR11**: Firmware build produces zero compiler errors and zero `-Wall` warnings
