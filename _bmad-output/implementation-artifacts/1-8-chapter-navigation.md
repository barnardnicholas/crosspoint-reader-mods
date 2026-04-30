# Story 1.8: Chapter Navigation (parseToc + MobiReaderChapterSelectionActivity)

Status: ready-for-dev

## Story

As a user reading an AZW3 book,
I want to jump directly to any chapter from an in-reader chapter list,
so that I can navigate the book the same way I can in the EPUB reader.

## Acceptance Criteria

1. A `MobiChapter` struct exists in `Mobi.h` with `std::string title` and `uint32_t virtualOffset`.
2. `Mobi::parseToc()` private method exists, called from `load()` for KF8 files.
3. `parseToc()` extracts chapter entries from the KF8 NCX INDX record: title string + virtual byte offset. Chapters are stored in `std::vector<MobiChapter> chapters` (private member).
4. `Mobi::getChapterCount()` public method returns `chapters.size()`.
5. `Mobi::getChapter(size_t index)` public method returns `const MobiChapter&`.
6. `MobiReaderChapterSelectionActivity` exists (`.h` + `.cpp`), following the `EpubReaderChapterSelectionActivity` pattern exactly: list display, scroll navigation, Confirm selects and returns a `ChapterResult` with a `pageIndex`, Back cancels.
7. `MobiReaderActivity` launches `MobiReaderChapterSelectionActivity` when the user presses `Button::Confirm` (long-hold or dedicated trigger — match the existing behaviour for other reader types). On return, jumps to the page corresponding to the selected chapter's `virtualOffset`.
8. For MOBI7 files (no KF8 INDX), `chapters` is empty and the chapter list is not offered (Confirm press does nothing or goes home — same as current MOBI7 behaviour).
9. `pio run` produces zero errors and zero `-Wall` warnings.

## Tasks / Subtasks

- [ ] Add `MobiChapter` struct and member declarations to `Mobi.h` (AC: #1, #3, #4, #5)
  - [ ] In the public section of `class Mobi`:
    ```cpp
    struct MobiChapter {
        std::string title;
        uint32_t    virtualOffset;  // Byte offset into the virtual flat-text stream
    };

    [[nodiscard]] size_t getChapterCount() const { return chapters.size(); }
    [[nodiscard]] const MobiChapter& getChapter(size_t index) const { return chapters[index]; }
    ```
  - [ ] In the private section:
    ```cpp
    std::vector<MobiChapter> chapters;
    bool parseToc();
    ```

- [ ] Implement `parseToc()` in `Mobi.cpp` (AC: #2, #3)
  - [ ] Called from `load()` after `buildVirtualOffsetTable()` succeeds, KF8 only:
    ```cpp
    if (mobiVariant == MobiVariant::KF8) {
        parseToc();  // Non-fatal: failure means no chapters, which is fine
    }
    ```
  - [ ] Locate the NCX INDX record index: read the KF8 MOBI header field at offset 160 (relative to MOBI magic in KF8 rec0), uint32 BE — this is the NCX record index relative to the KF8 section Record 0
  - [ ] If the NCX record index is 0xFFFFFFFF, no TOC exists — return true (empty chapters)
  - [ ] Read the NCX INDX record from `recordFileOffsets[kf8SectionRecord + ncxRecordIndex]`
  - [ ] INDX record structure:
    ```
    [0]   4 bytes  magic "INDX"
    [4]   4 bytes  header length (uint32 BE)
    [8]   4 bytes  record type: 0=INDX, 1=INDXT (uint32 BE)
    [12]  4 bytes  count of entries in this record (uint32 BE)
    [16]  4 bytes  encoding (uint32 BE) — 65001 = UTF-8
    [20]  4 bytes  index of next INDX record (uint32 BE) — 0xFFFFFFFF if last
    [24]  4 bytes  total entry count across all INDX records (uint32 BE)
    [28]  4 bytes  offset of tag-table section from record start (uint32 BE)
    [32]  4 bytes  offset of CNCX record (uint32 BE) — 0 if none
    [40]  4 bytes  offset of index entries section from record start (uint32 BE)
    [44]  2 bytes  number of index entries in this record (uint16 BE)
    ```
  - [ ] For each entry in the INDX record, extract:
    - Entry label (UTF-8 string, length-prefixed) → chapter title
    - Entry offset attribute → virtual byte offset into the text stream
  - [ ] INDX entry format (simplified — sufficient for title + offset extraction):
    ```
    [0]    1 byte   label length
    [1..N] N bytes  label (UTF-8) — the chapter title
    [N+1]  1 byte   number of tags
    [N+2]  tags: each is (tagID, values...) per INDX tag table
    ```
  - [ ] Tag ID 1 = file offset (in the raw/virtual text stream). Map this to a virtual byte offset
  - [ ] Push to `chapters` as `MobiChapter{title, virtualOffset}`
  - [ ] On any parse error: `LOG_ERR("MOBI", ...)`, clear `chapters`, return true (non-fatal)
  - [ ] **Simplification allowed:** If INDX parsing proves complex, fall back to extracting only the first INDX record (most books have all chapters in one INDX record). Full multi-INDX iteration can be deferred.

- [ ] Create `MobiReaderChapterSelectionActivity.h` (AC: #6)
  - [ ] In `src/activities/reader/`:
    ```cpp
    #pragma once
    #include <Mobi.h>
    #include <memory>
    #include "../Activity.h"
    #include "util/ButtonNavigator.h"

    class MobiReaderChapterSelectionActivity final : public Activity {
      std::shared_ptr<Mobi> mobi;   // NOTE: Mobi must be shareable — see Dev Notes
      int selectorIndex = 0;
      ButtonNavigator buttonNavigator;

      int getPageItems() const;

     public:
      struct ChapterResult {
          size_t virtualOffset;
          bool isCancelled = false;
      };

      explicit MobiReaderChapterSelectionActivity(GfxRenderer& renderer,
                                                  MappedInputManager& mappedInput,
                                                  std::shared_ptr<Mobi> mobi,
                                                  int currentChapterIndex)
          : Activity("MobiReaderChapterSelection", renderer, mappedInput),
            mobi(std::move(mobi)),
            selectorIndex(currentChapterIndex) {}

      void onEnter() override;
      void onExit() override;
      void loop() override;
      void render(RenderLock&&) override;
    };
    ```

- [ ] Create `MobiReaderChapterSelectionActivity.cpp` (AC: #6)
  - [ ] Follow `EpubReaderChapterSelectionActivity.cpp` pattern exactly
  - [ ] `getTotalItems()` → `mobi->getChapterCount()`
  - [ ] `getChapter(i).title` for list item text
  - [ ] On Confirm: `setResult(ChapterResult{mobi->getChapter(selectorIndex).virtualOffset}); finish();`
  - [ ] On Back: `setResult(ChapterResult{0, true}); finish();`
  - [ ] Draw using `GUI.drawList()` with the chapter titles
  - [ ] Call `menuDisplay()` at end of render
  - [ ] Include `STR_SELECT_CHAPTER` for the screen title (already exists in i18n)

- [ ] Wire chapter nav into `MobiReaderActivity` (AC: #7, #8)
  - [ ] **First:** Verify how `MobiReaderActivity` currently handles `Button::Confirm` in `loop()`.
    If it already has a long-press or menu trigger, reuse that pattern. If not, add:
    ```cpp
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        if (mobi && mobi->getChapterCount() > 0) {
            // Find current chapter index from pageOffsets[currentPage]
            const size_t currentOffset = pageOffsets[currentPage];
            int currentChapter = 0;
            for (size_t i = 0; i < mobi->getChapterCount(); i++) {
                if (mobi->getChapter(i).virtualOffset <= currentOffset) currentChapter = i;
            }
            // Launch chapter selection
            // See Dev Notes for Mobi ownership pattern
        }
    }
    ```
  - [ ] On `ChapterResult` return (not cancelled): convert `virtualOffset` to page index:
    ```cpp
    // Binary search pageOffsets for the page containing virtualOffset
    int page = 0;
    for (int i = 0; i < totalPages - 1; i++) {
        if (pageOffsets[i + 1] <= result.virtualOffset) page = i + 1;
        else break;
    }
    currentPage = page;
    requestUpdate();
    ```
  - [ ] If `mobi->getChapterCount() == 0` (MOBI7 or KF8 without NCX): Confirm does nothing (same as current)

- [ ] Handle `Mobi` ownership for chapter selection (AC: #6, #7)
  - [ ] `MobiReaderActivity` currently holds `std::unique_ptr<Mobi>`. The chapter selection activity needs to read chapter data from the same `Mobi` object.
  - [ ] **Option A (recommended):** Change `MobiReaderActivity` to hold `std::shared_ptr<Mobi>` and pass a shared copy to `MobiReaderChapterSelectionActivity`. Update `ReaderActivity.cpp` where `Mobi` is constructed.
  - [ ] **Option B (simpler):** Pass a raw pointer to the chapter selection activity (valid as long as `MobiReaderActivity` is on the stack). Mark with a comment that the pointer is non-owning and outlives the selection activity.
  - [ ] Choose Option B if the activity stack guarantees `MobiReaderActivity` outlives the chapter selection (which it does — the selection is pushed on top). Use `Mobi* mobi` (raw, non-owning) in `MobiReaderChapterSelectionActivity` and document why.

- [ ] Verify build (AC: #9)
  - [ ] Run `pio run` — confirm zero errors, zero warnings

## Dev Notes

**INDX parsing complexity:** KF8 INDX records use a tag-table structure that can be complex to parse fully. For MVP, only extract the chapter title (label field) and the text offset tag (tag ID 1). If the tag table parsing is unclear, implement a minimal parser that reads the label string and scans for tag ID 1 — sufficient for the user-facing feature. The architecture doc notes: "Chapter nav can be deferred to Growth if unexpectedly complex."

**NCX record field location:** The KF8 MOBI header field for the NCX record index is at byte 160 relative to the MOBI magic in KF8 Record 0 (absolute offset = KF8Rec0FileOffset + 16 + 160 = KF8Rec0FileOffset + 176). This is the "NCXIndex" field in the MOBI header. 0xFFFFFFFF means no NCX.

**Virtual offset from INDX:** The offset stored in INDX tag 1 is a byte offset into the *concatenated decompressed text* of all KF8 text records (i.e., the rawML). This maps directly to the virtual offset table built by `buildVirtualOffsetTable()`. No conversion needed.

**`parseToc()` non-fatal:** A parse failure must not abort `load()`. `chapters` is left empty; chapter nav is simply unavailable. `LOG_ERR` the failure reason.

**Mobi ownership — raw pointer is safe:** The activity stack in CrossPoint pushes `MobiReaderChapterSelectionActivity` on top of `MobiReaderActivity`. `MobiReaderActivity::onExit()` is not called until after `MobiReaderChapterSelectionActivity::onExit()`. A raw `Mobi*` in the chapter activity is safe for the activity's full lifetime. No shared_ptr overhead.

**Button Confirm behaviour:** Check `MobiReaderActivity::loop()` — currently handles `Button::Back` for nav but not `Button::Confirm`. Add Confirm handling. Verify the `MappedInputManager::Button::Confirm` enum maps to the correct physical button in the MOBI reader context (it maps to the same button as in other activities).

**`STR_SELECT_CHAPTER`** already exists in the i18n system (used by `EpubReaderChapterSelectionActivity`). No new i18n keys needed.

**`ButtonNavigator`** — copy the exact usage from `EpubReaderChapterSelectionActivity.cpp`. It handles held-button continuous scroll.

### Project Structure Notes

- Modified files: `lib/Mobi/Mobi.h`, `lib/Mobi/Mobi.cpp`, `src/activities/reader/MobiReaderActivity.cpp`, `src/activities/reader/MobiReaderActivity.h`
- New files: `src/activities/reader/MobiReaderChapterSelectionActivity.h`, `src/activities/reader/MobiReaderChapterSelectionActivity.cpp`

### References

- Architecture: Implementation Sequence item 8 — [architecture.md]
- Architecture gap: "parseToc() populates the existing chapter struct type. Whether MobiReaderActivity already has a chapter list trigger…must be verified before implementing." — [architecture.md § "Gap Analysis"]
- Research doc: "Chapter navigation (NCX/TOC): KF8 stores an NCX-like index via INDX records."
- Reference pattern: `src/activities/reader/EpubReaderChapterSelectionActivity.cpp`
- Reference pattern: `src/activities/reader/XtcReaderChapterSelectionActivity.cpp`
- `MobiReaderActivity::loop()`: `src/activities/reader/MobiReaderActivity.cpp`
- KF8 INDX format: [MobileRead Wiki — KF8](https://wiki.mobileread.com/wiki/KF8)
- Prerequisite: Story 1.7 (KF8 readContent dispatch working; load() succeeds for AZW3)

## Dev Agent Record

### Agent Model Used

### Debug Log References

### Completion Notes List

### File List
