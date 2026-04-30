#pragma once

#include <HalStorage.h>

#include <string>
#include <vector>

/**
 * MOBI e-book format reader.
 *
 * Supports PalmDOC-compressed (type 2), uncompressed (type 1), and
 * Huffman/CDIC-compressed (type 17480 / KF8) MOBI files.
 *
 * Presents a "virtual flat file" interface: readContent() accepts byte offsets
 * into the decompressed, HTML-stripped text stream. This lets MobiReaderActivity
 * reuse the same pagination logic as TxtReaderActivity without modification.
 *
 * Virtual offset table (voffsets.bin in cache dir) maps text record index →
 * cumulative stripped-text byte count, enabling O(log n) random access.
 */
class Mobi {
 public:
  enum class MobiError : uint8_t { None = 0, DrmProtected = 1, CdicCapExceeded = 2, MalformedHeader = 3 };

  struct MobiChapter {
    std::string title;
    uint32_t virtualOffset;
  };

  explicit Mobi(std::string path, std::string cacheBasePath);
  ~Mobi();

  /**
   * Full load: parse headers + build/load virtual offset table.
   * Call this before readContent() or getVirtualSize().
   */
  bool load();

  /**
   * Header-only load: parse PalmDB/MOBI headers for metadata (title, author).
   * Does NOT build the virtual offset table. Suitable for the recent-books
   * metadata scan on boot (fast, no record scanning).
   */
  bool loadHeader();

  [[nodiscard]] const std::string& getPath() const { return filepath; }
  [[nodiscard]] const std::string& getCachePath() const { return cachePath; }
  [[nodiscard]] std::string getTitle() const;
  [[nodiscard]] const std::string& getAuthor() const { return author; }
  [[nodiscard]] size_t getVirtualSize() const { return virtualTextSize; }

  [[nodiscard]] MobiError getLastError() const { return lastError; }

  [[nodiscard]] size_t getChapterCount() const { return chapters.size(); }
  [[nodiscard]] const MobiChapter& getChapter(size_t index) const { return chapters[index]; }

  // Fast header-only DRM check. Opens the file, reads ~186 bytes, checks encryptionType.
  // Does NOT construct a Mobi object. Returns true if DRM-locked, false otherwise or on error.
  [[nodiscard]] static bool isDrmLocked(const char* path);

  void setupCacheDir() const;

  /**
   * Read decompressed, HTML-stripped MOBI text at a virtual byte offset.
   * `offset` and `length` refer to positions in the stripped text stream,
   * not raw file positions. Requires load() to have been called first.
   */
  [[nodiscard]] bool readContent(uint8_t* buffer, size_t offset, size_t length) const;

  /**
   * Keep the .mobi file open across multiple readContent() calls.
   * Avoids repeated FAT32 file opens during sequential access (e.g. page-index building).
   * openStream() failure is non-fatal: readContent() falls back to per-call open/close.
   * Must be paired with closeStream() when sequential access is complete.
   */
  bool openStream();
  void closeStream();

  // Cover image support — looks for cover.bmp/jpg/jpeg/png in same folder as .mobi file
  [[nodiscard]] std::string getCoverBmpPath() const;
  [[nodiscard]] bool generateCoverBmp() const;
  [[nodiscard]] std::string findCoverImage() const;
  [[nodiscard]] std::string getThumbBmpPath() const { return ""; }
  [[nodiscard]] bool generateThumbBmp(int /*height*/) const { return false; }

 private:
  enum class MobiVariant : uint8_t { MOBI7 = 0, KF8 = 1 };

  static constexpr uint8_t MAX_CDIC_RECORDS = 10;
  static constexpr size_t CDIC_RECORD_SIZE = 4096;

  struct HuffTable {
    uint32_t dict1[256];  // codelen(5b)|term(1b)|maxcode(24b), big-endian from file
    uint32_t dict2[64];   // mincode/maxcode pairs, big-endian from file
  };

  struct CdicTable {
    uint8_t* records[MAX_CDIC_RECORDS] = {};
    uint8_t recordCount = 0;
    uint16_t phrasesPerRecord = 0;
  };

  HuffTable huffTable{};
  CdicTable cdicTable{};
  bool huffCdicLoaded = false;

  std::string filepath;
  std::string cacheBasePath;
  std::string cachePath;

  bool headerLoaded = false;
  bool loaded = false;
  uint32_t fileSize = 0;

  // Persistent file handle for sequential reads (openStream / closeStream).
  // mutable so readContent() (which is const) can use it without reopening.
  mutable FsFile streamFile;
  mutable bool streamOpen = false;

  // Populated by loadHeader()
  std::string title;   // From MOBI full-name field or database name
  std::string author;  // From EXTH record type 100

  uint16_t compressionType = 0;    // 1=none, 2=PalmDOC, 17480=Huffman
  uint16_t textRecordCount = 0;    // Number of text data records
  uint16_t maxRecordSize = 0;      // Max uncompressed bytes per record (typically 4096)
  uint32_t rawTextLength = 0;      // Uncompressed text length from PalmDOC header
  uint16_t extraDataFlags = 0;     // Trailing-byte flags from MOBI header offset 242

  // Format detection — populated by parseMobiHeaders() and detectFormat()
  MobiVariant mobiVariant = MobiVariant::MOBI7;
  uint32_t kf8SectionRecord = 0xFFFFFFFF;  // Record index of KF8 section (0xFFFFFFFF = no KF8)
  uint32_t mobiTypeField = 0;              // Cached MOBI type field from rec0 offset 24
  uint32_t kf8FirstTextRecord = 1;         // PalmDB index of first KF8 text record (1 for MOBI7)
  MobiError lastError = MobiError::None;

  // File offsets of ALL PalmDB records (record 0 is the header record;
  // text records are 1 .. textRecordCount).
  std::vector<uint32_t> recordFileOffsets;

  // Virtual offset table: virtualOffsets[i] = virtual byte offset at the
  // START of stripped text from text record i (0-indexed within text records).
  // Has textRecordCount+1 entries; the last entry equals virtualTextSize.
  std::vector<uint32_t> virtualOffsets;
  uint32_t virtualTextSize = 0;

  std::vector<MobiChapter> chapters;

  // --- Parsing helpers ---
  bool parsePalmDbHeader(FsFile& file);
  bool parseMobiHeaders(FsFile& file);
  bool detectFormat();
  bool loadHuffCdic();
  size_t decompressHuffCdic(const uint8_t* in, size_t inLen, uint8_t* out, size_t outMax) const;
  bool parseToc();

  // --- Virtual offset table ---
  bool buildVirtualOffsetTable();
  bool loadVirtualOffsetTable();
  bool saveVirtualOffsetTable() const;

  // --- Per-record processing ---
  // Read raw bytes of text record `recIdx` (0-based within text records) into buf.
  // Returns number of bytes read (after trailing-byte stripping), or 0 on error.
  // buf must be at least maxRecordSize bytes.
  size_t readRawRecord(FsFile& file, uint16_t recIdx, uint8_t* buf, size_t bufSize) const;

  // Strip trailing bytes from a raw record buffer per extraDataFlags.
  // Returns the effective record size after stripping.
  size_t stripTrailingBytes(const uint8_t* buf, size_t rawSize) const;

  // PalmDOC decompressor. Returns number of bytes written to out, or 0 on error.
  // out must be >= maxRecordSize*2 bytes (PalmDOC expands at most ~4x, typically <2x).
  static size_t decompressPalmDoc(const uint8_t* in, size_t inLen, uint8_t* out, size_t outMax);

  // Strip HTML tags from input, writing plain text to output (may be same buffer for in-place).
  // Converts block-level tags to newlines; decodes common HTML entities.
  // Returns number of bytes written to output.
  static size_t stripHtml(const uint8_t* in, size_t inLen, uint8_t* out, size_t outMax);

  // Big-endian read helpers (avoids alignment faults on RISC-V)
  static uint16_t readU16BE(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) | p[1];
  }
  static uint32_t readU32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
  }
};
