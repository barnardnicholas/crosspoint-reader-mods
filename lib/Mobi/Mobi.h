#pragma once

#include <HalStorage.h>

#include <string>
#include <vector>

/**
 * MOBI e-book format reader.
 *
 * Supports PalmDOC-compressed (type 2) and uncompressed (type 1) MOBI files.
 * Huffman-compressed (type 17480 / KF8) files are not supported.
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
  explicit Mobi(std::string path, std::string cacheBasePath);

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

  // File offsets of ALL PalmDB records (record 0 is the header record;
  // text records are 1 .. textRecordCount).
  std::vector<uint32_t> recordFileOffsets;

  // Virtual offset table: virtualOffsets[i] = virtual byte offset at the
  // START of stripped text from text record i (0-indexed within text records).
  // Has textRecordCount+1 entries; the last entry equals virtualTextSize.
  std::vector<uint32_t> virtualOffsets;
  uint32_t virtualTextSize = 0;

  // --- Parsing helpers ---
  bool parsePalmDbHeader(FsFile& file);
  bool parseMobiHeaders(FsFile& file);

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
