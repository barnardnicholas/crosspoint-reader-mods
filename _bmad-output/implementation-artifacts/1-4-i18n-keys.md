# Story 1.4: i18n Keys for Error Messages

Status: review

## Story

As a user who opens a DRM-protected or unsupported Kindle file,
I want to see a clear, localised error message,
so that I understand why the file cannot be opened.

## Acceptance Criteria

1. `STR_DRM_PROTECTED` key exists in all 20 translation YAML files in `lib/I18n/translations/`.
2. `STR_CDIC_CAP_EXCEEDED` key exists in all 20 translation YAML files.
3. English values: `STR_DRM_PROTECTED: "This file is DRM-protected and cannot be opened."` and `STR_CDIC_CAP_EXCEEDED: "This file uses too many compression tables and cannot be opened."`.
4. All other language files contain the same keys with the English value as a placeholder (i18n standard for untranslated strings — they fall back to English at runtime).
5. After running `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/`, the generated headers compile without errors (`pio run`).
6. The `STR_DRM_PROTECTED` and `STR_CDIC_CAP_EXCEEDED` enum values are available in the generated `I18nKeys.h`.

## Tasks / Subtasks

- [x] Add keys to `english.yaml` (AC: #1, #2, #3)
  - [x] Added `STR_DRM_PROTECTED` and `STR_CDIC_CAP_EXCEEDED` to end of `english.yaml`
- [x] Add keys to all other language YAML files (AC: #4)
  - [x] Added both keys with English placeholder strings to all 19 non-English YAML files
- [x] Regenerate i18n headers (AC: #5, #6)
  - [x] Ran: `python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/` — 291 keys, 19 languages
  - [x] Verified `STR_DRM_PROTECTED` and `STR_CDIC_CAP_EXCEEDED` appear in generated `I18nKeys.h`
- [x] Verify build (AC: #5)
  - [x] Run `pio run` — SUCCESS, zero errors, zero warnings

## Dev Notes

**The generated files are gitignored** — `I18nKeys.h`, `I18nStrings.h`, `I18nStrings.cpp` are in `.gitignore` and regenerated at build time. Only commit the YAML source files.

**YAML source files to commit:** All 20 `lib/I18n/translations/*.yaml` files — these are the only files to commit for this story.

**i18n system:** The `tr(STR_DRM_PROTECTED)` macro (from `I18n.h`) returns a `const char*` for the current language at runtime. These string IDs are used by `ReaderActivity` in story 1.5 to pass to `FullScreenMessageActivity`.

**Existing pattern to follow** for new keys — look at how `STR_MEMORY_ERROR` and `STR_PAGE_LOAD_ERROR` are used in the existing codebase:
```cpp
#include <I18n.h>
// Usage:
tr(STR_MEMORY_ERROR)  // returns const char*
```

**Do not add i18n usage in C++ in this story** — that happens in story 1.5 (`ReaderActivity` error routing).

**Key naming convention** (from existing YAML): `STR_` prefix, UPPER_SNAKE_CASE, descriptive noun phrase.

**YAML format** — check an existing file for exact format. Keys are simple `KEY: "value"` pairs. No YAML anchors needed.

### Project Structure Notes

- Modified files: all 20 `lib/I18n/translations/*.yaml` files
- Generated files (NOT committed): `lib/I18n/I18nKeys.h`, `lib/I18n/I18nStrings.h`, `lib/I18n/I18nStrings.cpp`
- No new source files

### References

- Architecture: Implementation Sequence item 4 — [architecture.md]
- i18n workflow: `CLAUDE.md` § "To add/modify translations (i18n)"
- Generator script: `scripts/gen_i18n.py`
- Existing error string example: `STR_MEMORY_ERROR` in `lib/I18n/translations/english.yaml`
- Usage pattern: `lib/I18n/I18n.h` (the `tr()` macro)

## Dev Agent Record

### Agent Model Used
claude-sonnet-4-6

### Debug Log References

### Completion Notes List
- Added `STR_DRM_PROTECTED` and `STR_CDIC_CAP_EXCEEDED` to all 20 translation YAML files
- All non-English files use English placeholder strings (i18n fallback pattern)
- Regenerated: `I18nKeys.h`, `I18nStrings.h`, `I18nStrings.cpp` — 291 total keys
- Build: SUCCESS — zero errors, zero warnings

### File List
- lib/I18n/translations/english.yaml
- lib/I18n/translations/belarusian.yaml
- lib/I18n/translations/catalan.yaml
- lib/I18n/translations/czech.yaml
- lib/I18n/translations/danish.yaml
- lib/I18n/translations/dutch.yaml
- lib/I18n/translations/finnish.yaml
- lib/I18n/translations/french.yaml
- lib/I18n/translations/german.yaml
- lib/I18n/translations/italian.yaml
- lib/I18n/translations/kazakh.yaml
- lib/I18n/translations/polish.yaml
- lib/I18n/translations/portuguese.yaml
- lib/I18n/translations/romanian.yaml
- lib/I18n/translations/russian.yaml
- lib/I18n/translations/spanish.yaml
- lib/I18n/translations/swedish.yaml
- lib/I18n/translations/turkish.yaml
- lib/I18n/translations/ukrainian.yaml
