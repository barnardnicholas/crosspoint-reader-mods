#include "MobiReaderActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Utf8.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;           // 8 KB read chunk
constexpr uint32_t CACHE_MAGIC = 0x4D424249;      // "MBBI"
constexpr uint8_t CACHE_VERSION = 1;
}  // namespace

void MobiReaderActivity::onEnter() {
  Activity::onEnter();

  if (!mobi) return;

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  mobi->setupCacheDir();

  const auto filePath = mobi->getPath();
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, mobi->getTitle(), mobi->getAuthor(), mobi->getCoverBmpPath());

  requestUpdate();
}

void MobiReaderActivity::onExit() {
  Activity::onExit();

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  ReaderUtils::fullRefreshOnExit(renderer);

  pageOffsets.clear();
  currentPageLines.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  mobi.reset();
}

void MobiReaderActivity::loop() {
  if (mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(mobi ? mobi->getPath() : "");
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) return;

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    requestUpdate();
  } else if (nextTriggered && currentPage < totalPages - 1) {
    currentPage++;
    requestUpdate();
  }
}

void MobiReaderActivity::initializeReader() {
  if (initialized) return;

  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight,
                                   &cachedOrientedMarginBottom, &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_DBG("MRS", "Viewport: %dx%d, lines per page: %d", viewportWidth, viewportHeight, linesPerPage);

  if (!loadPageIndexCache()) {
    buildPageIndex();
    savePageIndexCache();
  }

  loadProgress();
  initialized = true;
}

void MobiReaderActivity::buildPageIndex() {
  pageOffsets.clear();
  // Reserve based on rough estimate to avoid repeated realloc+copy during build.
  // ~1200 bytes of stripped text per page (22 lines × ~55 chars); +1 for the initial offset.
  pageOffsets.reserve(mobi->getVirtualSize() / 1200 + 1);
  pageOffsets.push_back(0);

  size_t offset = 0;
  const size_t virtualSize = mobi->getVirtualSize();

  LOG_DBG("MRS", "Building page index for %zu virtual bytes...", virtualSize);

  GUI.drawPopup(renderer, tr(STR_INDEXING));

  // Keep the .mobi file open for the duration of index building.
  // Without this, readContent() reopens the file on every page (~900+ opens for a large book).
  if (!mobi->openStream()) {
    LOG_DBG("MRS", "Stream open failed; falling back to per-call file opens");
  }

  // Reuse a single vector across iterations to avoid repeated heap allocation.
  std::vector<std::string> tempLines;
  tempLines.reserve(static_cast<size_t>(linesPerPage));

  while (offset < virtualSize) {
    size_t nextOffset = offset;

    if (!loadPageAtOffset(offset, tempLines, nextOffset)) break;
    if (nextOffset <= offset) break;

    offset = nextOffset;
    if (offset < virtualSize) {
      pageOffsets.push_back(offset);
    }

    if (pageOffsets.size() % 20 == 0) {
      vTaskDelay(1);
    }
  }

  mobi->closeStream();

  totalPages = pageOffsets.size();
  LOG_DBG("MRS", "Built page index: %d pages", totalPages);
}

bool MobiReaderActivity::loadPageAtOffset(size_t offset, std::vector<std::string>& outLines,
                                          size_t& nextOffset) {
  outLines.clear();
  const size_t virtualSize = mobi->getVirtualSize();

  if (offset >= virtualSize) return false;

  const size_t chunkSize = std::min(CHUNK_SIZE, virtualSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) {
    LOG_ERR("MRS", "Failed to allocate %zu bytes", chunkSize);
    return false;
  }

  if (!mobi->readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  size_t pos = 0;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') lineEnd++;

    const bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= virtualSize);

    if (!lineComplete && static_cast<int>(outLines.size()) > 0) break;

    const size_t lineContentLen = lineEnd - pos;
    const bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    const size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    std::string line(reinterpret_cast<char*>(buffer + pos), displayLen);
    size_t lineBytePos = 0;

    while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage) {
      // Single O(fitting-chars) scan: stops as soon as a glyph's right edge exceeds
      // viewportWidth. For a long source paragraph this is critical — getTextWidth
      // would scan the entire remaining string even if only ~55 chars fit.
      const size_t fittingBytes = renderer.measureTextFitting(cachedFontId, line.c_str(), viewportWidth);

      if (fittingBytes >= line.length()) {
        // Entire remaining source line fits on one visual line.
        outLines.push_back(line);
        lineBytePos = displayLen;
        line.clear();
        break;
      }

      // Backtrack from the fitting limit to the last word boundary.
      size_t breakPos = fittingBytes;
      if (breakPos > 0) {
        const size_t spacePos = line.rfind(' ', breakPos - 1);
        if (spacePos != std::string::npos && spacePos > 0) {
          breakPos = spacePos;
        } else {
          // No word boundary: break mid-word; align to a UTF-8 char boundary.
          while (breakPos > 0 && (line[breakPos] & 0xC0) == 0x80) breakPos--;
        }
      }

      if (breakPos == 0) breakPos = 1;

      outLines.push_back(line.substr(0, breakPos));

      size_t skipChars = breakPos;
      if (breakPos < line.length() && line[breakPos] == ' ') skipChars++;
      lineBytePos += skipChars;
      line = line.substr(skipChars);
    }

    if (line.empty()) {
      pos = lineEnd + 1;  // Consumed this source line; skip the newline
    } else {
      pos = pos + lineBytePos;  // Page full mid-line
      break;
    }
  }

  if (pos == 0 && !outLines.empty()) pos = 1;  // Safety: always make progress

  nextOffset = offset + pos;
  if (nextOffset > virtualSize) nextOffset = virtualSize;

  free(buffer);
  return !outLines.empty();
}

void MobiReaderActivity::render(RenderLock&&) {
  if (!mobi) return;

  if (!initialized) initializeReader();

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    ReaderUtils::applyDarkModeIfEnabled(renderer);
    renderer.displayBuffer();
    return;
  }

  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  const size_t offset = pageOffsets[currentPage];
  size_t nextOffset = 0;
  currentPageLines.clear();
  loadPageAtOffset(offset, currentPageLines, nextOffset);

  renderer.clearScreen();
  renderPage();
  saveProgress();
}

void MobiReaderActivity::renderPage() {
  const int lineHeight = renderer.getLineHeight(cachedFontId);
  const int contentWidth = viewportWidth;

  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (const auto& line : currentPageLines) {
      if (!line.empty()) {
        int x = cachedOrientedMarginLeft;

        switch (cachedParagraphAlignment) {
          case CrossPointSettings::LEFT_ALIGN:
          default:
            break;
          case CrossPointSettings::CENTER_ALIGN: {
            const int textWidth = renderer.getTextWidth(cachedFontId, line.c_str());
            x = cachedOrientedMarginLeft + (contentWidth - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
            const int textWidth = renderer.getTextWidth(cachedFontId, line.c_str());
            x = cachedOrientedMarginLeft + contentWidth - textWidth;
            break;
          }
          case CrossPointSettings::JUSTIFIED:
            // Plain-text justified falls back to left-aligned
            break;
        }

        renderer.drawText(cachedFontId, x, y, line.c_str());
      }
      y += lineHeight;
    }
  };

  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLines();  // Scan pass — accumulates glyph usage, no drawing
  scope.endScanAndPrewarm();

  renderLines();  // BW render pass
  renderStatusBar();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing && !SETTINGS.readerDarkMode) {
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
  }
}

void MobiReaderActivity::renderStatusBar() const {
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    title = mobi->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title);
}

void MobiReaderActivity::saveProgress() const {
  FsFile f;
  if (Storage.openFileForWrite("MRS", mobi->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = 0;
    data[3] = 0;
    f.write(data, 4);
    f.close();
  }
}

void MobiReaderActivity::loadProgress() {
  FsFile f;
  if (Storage.openFileForRead("MRS", mobi->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= totalPages) currentPage = totalPages - 1;
      if (currentPage < 0) currentPage = 0;
      LOG_DBG("MRS", "Loaded progress: page %d/%d", currentPage, totalPages);
    }
    f.close();
  }
}

bool MobiReaderActivity::loadPageIndexCache() {
  const std::string cachePath = mobi->getCachePath() + "/index.bin";
  FsFile f;
  if (!Storage.openFileForRead("MRS", cachePath, f)) return false;

  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) { f.close(); return false; }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) { f.close(); return false; }

  uint32_t cachedVirtualSize;
  serialization::readPod(f, cachedVirtualSize);
  if (cachedVirtualSize != mobi->getVirtualSize()) { f.close(); return false; }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) { f.close(); return false; }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) { f.close(); return false; }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) { f.close(); return false; }

  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) { f.close(); return false; }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) { f.close(); return false; }

  uint32_t numPages;
  serialization::readPod(f, numPages);

  pageOffsets.clear();
  pageOffsets.reserve(numPages);
  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  f.close();
  totalPages = pageOffsets.size();
  LOG_DBG("MRS", "Loaded page index cache: %d pages", totalPages);
  return true;
}

void MobiReaderActivity::savePageIndexCache() const {
  const std::string cachePath = mobi->getCachePath() + "/index.bin";
  FsFile f;
  if (!Storage.openFileForWrite("MRS", cachePath, f)) {
    LOG_ERR("MRS", "Failed to save page index cache");
    return;
  }

  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(mobi->getVirtualSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  for (const size_t offset : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }

  f.close();
  LOG_DBG("MRS", "Saved page index cache: %d pages", totalPages);
}
