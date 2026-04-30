# Story 1.5: Format Dispatch — MobiVariant, detectFormat(), DRM Error Routing

Status: review

## Story

As a developer building KF8 support,
I want the Mobi class to identify the format variant of a file and expose a structured error code,
so that KF8-specific code paths can be dispatched correctly and DRM errors surface as user-readable messages.

## Acceptance Criteria

1. `MobiError` enum class exists in `Mobi.h` with values `None`, `DrmProtected`, `CdicCapExceeded`, `MalformedHeader`.
2. `Mobi::getLastError()` public method returns `MobiError`.
3. `Mobi::loadHeader()` detects `encryptionType == 2` at PalmDOC header offset 12 of Record 0, sets `lastError = MobiError::DrmProtected`, logs `LOG_ERR("MOBI", ...)`, and returns `false`. No decompression is attempted.
4. A private `detectFormat()` method exists. It reads the MOBI type field (Record 0 + MOBI header, offset from MOBI magic) and EXTH record 121. For MOBI7 files it returns `true` and sets `mobiVariant = MobiVariant::MOBI7`. For now, KF8 detection (type field == 248 or EXTH 121 != 0xFFFFFFFF) sets `mobiVariant = MobiVariant::KF8` and stores `kf8SectionRecord`, but the KF8 load path still returns `false` with `LOG_ERR` (KF8 reading not yet implemented — that is story 1.6/1.7).
5. `detectFormat()` is called from `load()` after `loadHeader()` succeeds.
6. `ReaderActivity::loadMobi()` is updated to check `mobi->getLastError()` on failure and show `FullScreenMessageActivity` with the appropriate i18n string (`STR_DRM_PROTECTED`, `STR_CDIC_CAP_EXCEEDED`, or generic fallback).
7. DRM-locked `.azw` file: file browser shows `[DRM]` (story 1.3), and opening it shows `FullScreenMessageActivity` with the DRM message. Device returns cleanly to file browser.
8. `pio run` produces zero errors and zero `-Wall` warnings.

## Tasks / Subtasks

- [x] Add `MobiError` enum and `MobiVariant` to `Mobi.h` (AC: #1)
  - [x] Added `MobiError` public enum with None/DrmProtected/CdicCapExceeded/MalformedHeader
  - [x] Added `MobiVariant` private enum with MOBI7/KF8
- [x] Add new private members to `Mobi.h` (AC: #4)
  - [x] Added `mobiVariant`, `kf8SectionRecord=0xFFFFFFFF`, `mobiTypeField=0`, `lastError=MobiError::None`
- [x] Add `getLastError()` public method to `Mobi.h` (AC: #2)
- [x] Add `detectFormat()` private declaration to `Mobi.h` (AC: #4)
- [x] Add DRM check to `Mobi::parseMobiHeaders()` in `Mobi.cpp` (AC: #3)
  - [x] DRM check fires before Huffman check; Huffman rejection moved to detectFormat()
  - [x] Removed Huffman rejection from `loadHeader()`
- [x] Implement `detectFormat()` in `Mobi.cpp` (AC: #4, #5)
  - [x] Reads mobiTypeField (cached in parseMobiHeaders) and kf8SectionRecord (EXTH 121)
  - [x] KF8 detection sets mobiVariant=KF8 and returns false (temporary stub for story 1.7)
  - [x] Huffman-compressed MOBI7 returns false with LOG_ERR
- [x] Wire `detectFormat()` into `load()` (AC: #5)
- [x] Update `ReaderActivity::loadMobi()` (AC: #6, #7)
  - [x] Made non-static to access activityManager/renderer/mappedInput
  - [x] Switch on getLastError() for DRM/CDIC user messages via FullScreenMessageActivity
  - [x] Added `<I18n.h>` include
- [x] Verify build (AC: #8)
  - [x] `pio run` — SUCCESS, zero errors, zero warnings

## Dev Notes

**MOBI type field location in Record 0:**
- MOBI magic at offset 16 from rec0 start
- MOBI header begins at offset 16
- MOBI type (KF8 = 248) is at MOBI header offset 8 → absolute rec0 offset 24
- Constants already defined: `REC0_MOBI_MAGIC_OFFSET = 16`, `REC0_MOBI_HEADER_LEN_OFFSET = 20`
- Add: `constexpr size_t REC0_MOBI_TYPE_OFFSET = 24;`

**EXTH record 121 (KF8 boundary):**
- EXTH parsing already exists in `parseMobiHeaders()` (reads types 100, 503)
- Extend the EXTH loop to also read type 121:
  ```cpp
  constexpr uint32_t EXTH_KF8_BOUNDARY = 121;
  // In EXTH loop:
  } else if (recType == EXTH_KF8_BOUNDARY) {
      if (dataLen == 4) {
          uint32_t boundary;
          memcpy(&boundary, buf + dataStart, 4);
          kf8SectionRecord = __builtin_bswap32(boundary);
      }
  }
  ```
- Store result in `kf8SectionRecord` member (already declared in this story)

**`detectFormat()` re-reads rec0:** The simplest approach is to re-open the file and re-read rec0 to get the type field. Alternatively, cache the MOBI type during `parseMobiHeaders()`. The latter is cleaner — add `uint32_t mobiType = 0;` as a member and set it in `parseMobiHeaders()`.

**`MobiError` scoping:** `MobiError` is declared in the public section of `Mobi` for use by `ReaderActivity`. `MobiVariant` is private (internal only).

**`FullScreenMessageActivity` usage:** Look at how other activities use it — it takes a message string and shows it full-screen. User presses Back to dismiss. The existing `onGoBack()` path in `ReaderActivity::onEnter()` handles the `nullptr` return from `loadMobi()` by calling `onGoBack()` which calls `finish()` — this returns to the file browser cleanly. Verify this is correct for the DRM case.

**Do NOT implement KF8 loading in this story** — `detectFormat()` detects KF8 and returns `false` with a LOG_ERR. Full KF8 support is stories 1.6 and 1.7.

**Prerequisite:** Story 1.4 must be complete (i18n keys `STR_DRM_PROTECTED`, `STR_CDIC_CAP_EXCEEDED` available).

### Project Structure Notes

- Modified files: `lib/Mobi/Mobi.h`, `lib/Mobi/Mobi.cpp`, `src/activities/reader/ReaderActivity.cpp`
- No new files

### References

- Architecture: Implementation Sequence items 5 — [architecture.md]
- Architecture: Error Routing pattern — [architecture.md § "Security & Error Handling"]
- `parseMobiHeaders()`: `lib/Mobi/Mobi.cpp:392-506`
- `loadHeader()` Huffman rejection: `lib/Mobi/Mobi.cpp:109-112` (to be removed and moved to `detectFormat()`)
- `ReaderActivity::loadMobi()`: `src/activities/reader/ReaderActivity.cpp:37-50`
- EXTH record type 121: research doc § "EXTH block starts…"

## Dev Agent Record

### Agent Model Used
claude-sonnet-4-6

### Debug Log References
None

### Completion Notes List
- `loadMobi` changed from static to non-static to allow activityManager/renderer/mappedInput access
- `mobiTypeField` cached in `parseMobiHeaders()` so `detectFormat()` needs no file re-open
- EXTH 121 parsed in EXTH loop alongside types 100 and 503
- DRM check inserted at encryptionType offset 12 in parseMobiHeaders(), fires before MOBI magic parse
- detectFormat() is temporary stub — KF8 path returns false with LOG_ERR until story 1.7

### File List
- lib/Mobi/Mobi.h
- lib/Mobi/Mobi.cpp
- src/activities/reader/ReaderActivity.h
- src/activities/reader/ReaderActivity.cpp
