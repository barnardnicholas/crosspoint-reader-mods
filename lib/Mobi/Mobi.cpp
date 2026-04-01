#include "Mobi.h"

#include <FsHelpers.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

namespace {

// Cache file identifiers
constexpr uint32_t VOFFSET_MAGIC = 0x4D424F49;  // "MBOI"
constexpr uint8_t VOFFSET_VERSION = 1;

// PalmDB record list entry size (4-byte offset + 4-byte attrs/UID)
constexpr size_t PALMDB_RECORD_ENTRY_SIZE = 8;

// PalmDB header size (fixed)
constexpr size_t PALMDB_HEADER_SIZE = 78;

// Offset of record count within PalmDB header
constexpr size_t PALMDB_RECORD_COUNT_OFFSET = 76;

// Offsets within Record 0 (relative to start of record 0 data)
constexpr size_t REC0_COMPRESSION_OFFSET = 0;
constexpr size_t REC0_TEXT_LENGTH_OFFSET = 4;
constexpr size_t REC0_RECORD_COUNT_OFFSET = 8;
constexpr size_t REC0_RECORD_SIZE_OFFSET = 10;
constexpr size_t REC0_MOBI_MAGIC_OFFSET = 16;

// Offsets within the MOBI header section of record 0
// (absolute offsets from start of record 0 data)
constexpr size_t REC0_MOBI_HEADER_LEN_OFFSET = 20;  // uint32 BE: length of MOBI header from "MOBI"
constexpr size_t REC0_TEXT_ENCODING_OFFSET = 28;
constexpr size_t REC0_FULLNAME_OFFSET = 84;          // uint32 BE: offset from rec0 start
constexpr size_t REC0_FULLNAME_LEN_OFFSET = 88;      // uint32 BE
constexpr size_t REC0_EXTH_FLAGS_OFFSET = 116;       // uint32 BE: bit 6 = has EXTH
constexpr size_t REC0_EXTRA_DATA_FLAGS_OFFSET = 242; // uint16 BE: trailing-byte flags

// Maximum bytes to read from record 0 for header parsing
constexpr size_t REC0_READ_SIZE = 2048;

// EXTH record types
constexpr uint32_t EXTH_AUTHOR = 100;
constexpr uint32_t EXTH_UPDATED_TITLE = 503;

// Compression types
constexpr uint16_t COMPRESSION_NONE = 1;
constexpr uint16_t COMPRESSION_PALMDOC = 2;
constexpr uint16_t COMPRESSION_HUFFMAN = 17480;

}  // namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

Mobi::Mobi(std::string path, std::string cacheBasePath)
    : filepath(std::move(path)), cacheBasePath(std::move(cacheBasePath)) {
  const size_t hash = std::hash<std::string>{}(filepath);
  cachePath = this->cacheBasePath + "/mobi_" + std::to_string(hash);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool Mobi::load() {
  if (loaded) return true;
  if (!loadHeader()) return false;

  // Try loading cached virtual offset table first; build if absent.
  if (!loadVirtualOffsetTable()) {
    if (!buildVirtualOffsetTable()) {
      LOG_ERR("MOBI", "Failed to build virtual offset table");
      return false;
    }
    saveVirtualOffsetTable();
  }

  loaded = true;
  LOG_DBG("MOBI", "Loaded: %s (%u virtual bytes, %u text records)", filepath.c_str(), virtualTextSize,
          textRecordCount);
  return true;
}

bool Mobi::loadHeader() {
  if (headerLoaded) return true;

  if (!Storage.exists(filepath.c_str())) {
    LOG_ERR("MOBI", "File does not exist: %s", filepath.c_str());
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("MOBI", filepath, file)) {
    LOG_ERR("MOBI", "Failed to open: %s", filepath.c_str());
    return false;
  }

  fileSize = static_cast<uint32_t>(file.size());

  bool ok = parsePalmDbHeader(file) && parseMobiHeaders(file);
  file.close();

  if (!ok) return false;

  if (compressionType == COMPRESSION_HUFFMAN) {
    LOG_ERR("MOBI", "Huffman (KF8) compression is not supported");
    return false;
  }

  headerLoaded = true;
  LOG_DBG("MOBI", "Header loaded: title='%s' author='%s' compression=%u records=%u", title.c_str(),
          author.c_str(), compressionType, textRecordCount);
  return true;
}

std::string Mobi::getTitle() const {
  if (!title.empty()) return title;

  // Fall back to filename without extension
  size_t lastSlash = filepath.find_last_of('/');
  std::string filename = (lastSlash != std::string::npos) ? filepath.substr(lastSlash + 1) : filepath;
  if (FsHelpers::hasMobiExtension(filename)) {
    filename = filename.substr(0, filename.length() - 5);
  }
  return filename;
}

void Mobi::setupCacheDir() const {
  if (!Storage.exists(cacheBasePath.c_str())) {
    Storage.mkdir(cacheBasePath.c_str());
  }
  if (!Storage.exists(cachePath.c_str())) {
    Storage.mkdir(cachePath.c_str());
  }
}

bool Mobi::openStream() {
  if (streamOpen) return true;
  if (!Storage.openFileForRead("MOBI", filepath, streamFile)) return false;
  streamOpen = true;
  return true;
}

void Mobi::closeStream() {
  if (streamOpen) {
    streamFile.close();
    streamOpen = false;
  }
}

bool Mobi::readContent(uint8_t* buffer, size_t offset, size_t length) const {
  if (!loaded) {
    LOG_ERR("MOBI", "readContent called before load()");
    return false;
  }
  if (offset >= virtualTextSize || length == 0) return false;

  // Clamp to available data
  if (offset + length > virtualTextSize) {
    length = virtualTextSize - offset;
  }

  // Allocate working buffers: raw record + decompressed output
  // maxRecordSize is typically 4096; PalmDOC expansion is at most ~4x
  const size_t rawBufSize = maxRecordSize + 32;       // +32 for safety margin
  const size_t decompBufSize = maxRecordSize * 4 + 32;

  auto* rawBuf = static_cast<uint8_t*>(malloc(rawBufSize));
  if (!rawBuf) {
    LOG_ERR("MOBI", "malloc failed: %u bytes (rawBuf)", static_cast<unsigned>(rawBufSize));
    return false;
  }
  auto* decompBuf = static_cast<uint8_t*>(malloc(decompBufSize));
  if (!decompBuf) {
    free(rawBuf);
    LOG_ERR("MOBI", "malloc failed: %u bytes (decompBuf)", static_cast<unsigned>(decompBufSize));
    return false;
  }

  // Reuse the persistent stream file if open (avoids repeated FAT32 opens during index building).
  FsFile localFile;
  FsFile& file = streamOpen ? streamFile : localFile;
  if (!streamOpen) {
    if (!Storage.openFileForRead("MOBI", filepath, localFile)) {
      free(decompBuf);
      free(rawBuf);
      return false;
    }
  }

  // Binary search: find the first text record whose virtual range overlaps [offset, offset+length)
  size_t startRec = 0;
  {
    size_t lo = 0, hi = textRecordCount;
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      if (virtualOffsets[mid + 1] <= offset) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    startRec = lo;
  }

  size_t outPos = 0;

  for (size_t r = startRec; r < textRecordCount && outPos < length; r++) {
    const size_t recVirtualStart = virtualOffsets[r];
    const size_t recVirtualEnd = virtualOffsets[r + 1];
    const size_t recVirtualLen = recVirtualEnd - recVirtualStart;

    // Read and decompress the record
    const size_t rawSize = readRawRecord(file, static_cast<uint16_t>(r), rawBuf, rawBufSize);
    if (rawSize == 0) break;

    size_t strippedLen = 0;
    if (compressionType == COMPRESSION_PALMDOC) {
      strippedLen = decompressPalmDoc(rawBuf, rawSize, decompBuf, decompBufSize);
    } else {
      // Uncompressed: strip HTML directly from raw
      if (rawSize <= decompBufSize) {
        memcpy(decompBuf, rawBuf, rawSize);
        strippedLen = rawSize;
      }
    }

    // Strip HTML in-place (output is <= input)
    strippedLen = stripHtml(decompBuf, strippedLen, decompBuf, strippedLen);

    // Sanity-check against the virtual offset table
    if (strippedLen != recVirtualLen) {
      // The virtual offset table may differ slightly from actual (e.g. after HTML stripping
      // produces different results). Use the minimum to avoid out-of-bounds reads.
      strippedLen = std::min(strippedLen, recVirtualLen);
    }

    // Determine which portion of this record overlaps the requested range
    size_t copyStart = 0;
    if (recVirtualStart < offset) {
      copyStart = offset - recVirtualStart;
    }
    size_t copyEnd = strippedLen;
    if (recVirtualStart + strippedLen > offset + length) {
      copyEnd = offset + length - recVirtualStart;
    }

    if (copyEnd > copyStart) {
      const size_t copyLen = copyEnd - copyStart;
      memcpy(buffer + outPos, decompBuf + copyStart, copyLen);
      outPos += copyLen;
    }
  }

  if (!streamOpen) {
    localFile.close();
  }
  free(decompBuf);
  free(rawBuf);

  return outPos > 0;
}

// ---------------------------------------------------------------------------
// Cover image (look for sidecar image file, same as Txt)
// ---------------------------------------------------------------------------

std::string Mobi::findCoverImage() const {
  size_t lastSlash = filepath.find_last_of('/');
  std::string folder = (lastSlash != std::string::npos) ? filepath.substr(0, lastSlash) : "/";

  const std::string baseName = getTitle();
  const char* extensions[] = {".bmp", ".jpg", ".jpeg", ".png", ".BMP", ".JPG", ".JPEG", ".PNG"};

  for (const auto& ext : extensions) {
    std::string coverPath = folder + "/" + baseName + ext;
    if (Storage.exists(coverPath.c_str())) return coverPath;
  }

  const char* coverNames[] = {"cover", "Cover", "COVER"};
  for (const auto& name : coverNames) {
    for (const auto& ext : extensions) {
      std::string coverPath = folder + "/" + std::string(name) + ext;
      if (Storage.exists(coverPath.c_str())) return coverPath;
    }
  }
  return "";
}

std::string Mobi::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

bool Mobi::generateCoverBmp() const {
  if (Storage.exists(getCoverBmpPath().c_str())) return true;

  const std::string coverImagePath = findCoverImage();
  if (coverImagePath.empty()) return false;

  setupCacheDir();

  if (FsHelpers::hasBmpExtension(coverImagePath)) {
    FsFile src, dst;
    if (!Storage.openFileForRead("MOBI", coverImagePath, src)) return false;
    if (!Storage.openFileForWrite("MOBI", getCoverBmpPath(), dst)) {
      src.close();
      return false;
    }
    uint8_t copyBuf[1024];
    while (src.available()) {
      const size_t n = src.read(copyBuf, sizeof(copyBuf));
      dst.write(copyBuf, n);
    }
    src.close();
    dst.close();
    return true;
  }

  if (FsHelpers::hasJpgExtension(coverImagePath)) {
    FsFile coverJpg, coverBmp;
    if (!Storage.openFileForRead("MOBI", coverImagePath, coverJpg)) return false;
    if (!Storage.openFileForWrite("MOBI", getCoverBmpPath(), coverBmp)) {
      coverJpg.close();
      return false;
    }
    const bool ok = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp);
    coverJpg.close();
    coverBmp.close();
    if (!ok) {
      Storage.remove(getCoverBmpPath().c_str());
      LOG_ERR("MOBI", "Failed to convert JPG cover to BMP");
    }
    return ok;
  }

  return false;
}

// ---------------------------------------------------------------------------
// PalmDB header parsing
// ---------------------------------------------------------------------------

bool Mobi::parsePalmDbHeader(FsFile& file) {
  // Read the 78-byte PalmDB fixed header
  uint8_t hdr[PALMDB_HEADER_SIZE];
  if (!file.seek(0) || file.read(hdr, sizeof(hdr)) != sizeof(hdr)) {
    LOG_ERR("MOBI", "Failed to read PalmDB header");
    return false;
  }

  // Validate type/creator ("BOOK"/"MOBI" or "BOOK"/"TEXt" for PalmDOC)
  // bytes 60-63 = type, 64-67 = creator
  // We accept any creator containing text records — don't be strict here.

  const uint16_t numRecords = readU16BE(hdr + PALMDB_RECORD_COUNT_OFFSET);
  if (numRecords < 2) {
    LOG_ERR("MOBI", "Too few records: %u", numRecords);
    return false;
  }

  // Read the record list (8 bytes each, right after the header)
  // Allocate on heap: numRecords * 8 bytes could be several KB
  const size_t listSize = numRecords * PALMDB_RECORD_ENTRY_SIZE;
  auto* recList = static_cast<uint8_t*>(malloc(listSize));
  if (!recList) {
    LOG_ERR("MOBI", "malloc failed: %u bytes (record list)", static_cast<unsigned>(listSize));
    return false;
  }

  if (file.read(recList, listSize) != listSize) {
    free(recList);
    LOG_ERR("MOBI", "Failed to read record list");
    return false;
  }

  recordFileOffsets.clear();
  recordFileOffsets.reserve(numRecords);
  for (uint16_t i = 0; i < numRecords; i++) {
    recordFileOffsets.push_back(readU32BE(recList + i * PALMDB_RECORD_ENTRY_SIZE));
  }

  free(recList);
  return true;
}

// ---------------------------------------------------------------------------
// MOBI header parsing (reads from record 0)
// ---------------------------------------------------------------------------

bool Mobi::parseMobiHeaders(FsFile& file) {
  if (recordFileOffsets.size() < 2) {
    LOG_ERR("MOBI", "Not enough records");
    return false;
  }

  const uint32_t rec0Start = recordFileOffsets[0];
  const uint32_t rec0Size =
      static_cast<uint32_t>(std::min<uint32_t>(recordFileOffsets[1] - rec0Start, REC0_READ_SIZE));

  auto* buf = static_cast<uint8_t*>(malloc(rec0Size));
  if (!buf) {
    LOG_ERR("MOBI", "malloc failed: %u bytes (rec0)", rec0Size);
    return false;
  }

  if (!file.seek(rec0Start) || file.read(buf, rec0Size) != rec0Size) {
    free(buf);
    LOG_ERR("MOBI", "Failed to read record 0");
    return false;
  }

  // PalmDOC header fields (first 16 bytes of record 0)
  compressionType = readU16BE(buf + REC0_COMPRESSION_OFFSET);
  rawTextLength = readU32BE(buf + REC0_TEXT_LENGTH_OFFSET);
  textRecordCount = readU16BE(buf + REC0_RECORD_COUNT_OFFSET);
  maxRecordSize = readU16BE(buf + REC0_RECORD_SIZE_OFFSET);

  if (maxRecordSize == 0) maxRecordSize = 4096;  // Sensible default

  // Check for MOBI magic at offset 16
  if (rec0Size < REC0_MOBI_MAGIC_OFFSET + 4 || buf[REC0_MOBI_MAGIC_OFFSET] != 'M' ||
      buf[REC0_MOBI_MAGIC_OFFSET + 1] != 'O' || buf[REC0_MOBI_MAGIC_OFFSET + 2] != 'B' ||
      buf[REC0_MOBI_MAGIC_OFFSET + 3] != 'I') {
    // Plain PalmDOC — use database name (bytes 0-31 of file) as title
    // Already available in the PalmDB header that was read in parsePalmDbHeader().
    // Re-read the first 32 bytes for the database name.
    uint8_t dbName[32];
    if (file.seek(0) && file.read(dbName, sizeof(dbName)) == sizeof(dbName)) {
      dbName[31] = '\0';
      title = reinterpret_cast<char*>(dbName);
    }
    free(buf);
    return true;
  }

  // MOBI header: header length field at rec0[20]
  if (rec0Size < REC0_MOBI_HEADER_LEN_OFFSET + 4) {
    free(buf);
    LOG_ERR("MOBI", "Record 0 too small to contain MOBI header length");
    return false;
  }
  const uint32_t mobiHeaderLen = readU32BE(buf + REC0_MOBI_HEADER_LEN_OFFSET);

  // Full name (title)
  if (rec0Size >= REC0_FULLNAME_LEN_OFFSET + 4) {
    const uint32_t fnOffset = readU32BE(buf + REC0_FULLNAME_OFFSET);
    const uint32_t fnLen = readU32BE(buf + REC0_FULLNAME_LEN_OFFSET);
    if (fnLen > 0 && fnOffset + fnLen <= rec0Size) {
      title = std::string(reinterpret_cast<char*>(buf + fnOffset), fnLen);
    }
  }

  // Extra data flags (trailing bytes in text records)
  if (rec0Size >= REC0_EXTRA_DATA_FLAGS_OFFSET + 2) {
    extraDataFlags = readU16BE(buf + REC0_EXTRA_DATA_FLAGS_OFFSET);
    if (extraDataFlags != 0) {
      LOG_DBG("MOBI", "Extra data flags: 0x%04X", extraDataFlags);
    }
  }

  // EXTH block
  bool hasExth = false;
  if (rec0Size >= REC0_EXTH_FLAGS_OFFSET + 4) {
    const uint32_t exthFlags = readU32BE(buf + REC0_EXTH_FLAGS_OFFSET);
    hasExth = (exthFlags & 0x40) != 0;
  }

  if (hasExth) {
    // EXTH block starts immediately after the MOBI header.
    // MOBI header begins at offset 16 in rec0; mobiHeaderLen counts from "MOBI".
    const uint32_t exthStart = REC0_MOBI_MAGIC_OFFSET + mobiHeaderLen;

    if (exthStart + 12 <= rec0Size && buf[exthStart] == 'E' && buf[exthStart + 1] == 'X' &&
        buf[exthStart + 2] == 'T' && buf[exthStart + 3] == 'H') {
      // exthStart+4: total EXTH length (uint32 BE)
      // exthStart+8: record count (uint32 BE)
      const uint32_t exthRecordCount = readU32BE(buf + exthStart + 8);
      uint32_t pos = exthStart + 12;

      for (uint32_t i = 0; i < exthRecordCount && pos + 8 <= rec0Size; i++) {
        const uint32_t recType = readU32BE(buf + pos);
        const uint32_t recLen = readU32BE(buf + pos + 4);

        if (recLen < 8) break;  // Malformed record

        const uint32_t dataStart = pos + 8;
        const uint32_t dataLen = recLen - 8;

        if (dataStart + dataLen <= rec0Size) {
          if (recType == EXTH_AUTHOR && author.empty()) {
            author = std::string(reinterpret_cast<const char*>(buf + dataStart), dataLen);
          } else if (recType == EXTH_UPDATED_TITLE) {
            // Updated title overrides the full-name field
            title = std::string(reinterpret_cast<const char*>(buf + dataStart), dataLen);
          }
        }

        pos += recLen;
      }
    }
  }

  free(buf);
  return true;
}

// ---------------------------------------------------------------------------
// Virtual offset table
// ---------------------------------------------------------------------------

bool Mobi::buildVirtualOffsetTable() {
  if (textRecordCount == 0) {
    virtualOffsets.clear();
    virtualOffsets.push_back(0);
    virtualTextSize = 0;
    return true;
  }

  virtualOffsets.clear();
  virtualOffsets.reserve(textRecordCount + 1);
  virtualOffsets.push_back(0);

  const size_t rawBufSize = maxRecordSize + 32;
  const size_t decompBufSize = maxRecordSize * 4 + 32;

  auto* rawBuf = static_cast<uint8_t*>(malloc(rawBufSize));
  if (!rawBuf) {
    LOG_ERR("MOBI", "malloc failed: %u bytes (rawBuf build)", static_cast<unsigned>(rawBufSize));
    return false;
  }
  auto* decompBuf = static_cast<uint8_t*>(malloc(decompBufSize));
  if (!decompBuf) {
    free(rawBuf);
    LOG_ERR("MOBI", "malloc failed: %u bytes (decompBuf build)", static_cast<unsigned>(decompBufSize));
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("MOBI", filepath, file)) {
    free(decompBuf);
    free(rawBuf);
    return false;
  }

  for (uint16_t r = 0; r < textRecordCount; r++) {
    const size_t rawSize = readRawRecord(file, r, rawBuf, rawBufSize);
    size_t strippedLen = 0;

    if (rawSize > 0) {
      if (compressionType == COMPRESSION_PALMDOC) {
        const size_t decompLen = decompressPalmDoc(rawBuf, rawSize, decompBuf, decompBufSize);
        strippedLen = stripHtml(decompBuf, decompLen, decompBuf, decompLen);
      } else {
        // Uncompressed — strip HTML in-place
        memcpy(decompBuf, rawBuf, rawSize);
        strippedLen = stripHtml(decompBuf, rawSize, decompBuf, rawSize);
      }
    }

    virtualOffsets.push_back(virtualOffsets.back() + static_cast<uint32_t>(strippedLen));

    // Yield periodically so the watchdog doesn't fire on large books
    if ((r & 0x1F) == 0x1F) {
      vTaskDelay(1);
    }
  }

  file.close();
  free(decompBuf);
  free(rawBuf);

  virtualTextSize = virtualOffsets.back();
  LOG_DBG("MOBI", "Built virtual offset table: %u records, %u virtual bytes", textRecordCount,
          virtualTextSize);
  return true;
}

bool Mobi::loadVirtualOffsetTable() {
  const std::string cachefile = cachePath + "/voffsets.bin";
  FsFile f;
  if (!Storage.openFileForRead("MOBI", cachefile, f)) return false;

  // Header
  uint32_t magic = 0;
  uint8_t version = 0;
  uint32_t cachedFileSize = 0;
  uint16_t cachedRecordCount = 0;
  uint16_t cachedCompression = 0;
  uint16_t cachedExtraDataFlags = 0;

  if (f.read(&magic, 4) != 4 || f.read(&version, 1) != 1 || f.read(&cachedFileSize, 4) != 4 ||
      f.read(&cachedRecordCount, 2) != 2 || f.read(&cachedCompression, 2) != 2 ||
      f.read(&cachedExtraDataFlags, 2) != 2) {
    f.close();
    return false;
  }

  if (magic != VOFFSET_MAGIC || version != VOFFSET_VERSION || cachedFileSize != fileSize ||
      cachedRecordCount != textRecordCount || cachedCompression != compressionType ||
      cachedExtraDataFlags != extraDataFlags) {
    LOG_DBG("MOBI", "Virtual offset cache invalid — will rebuild");
    f.close();
    return false;
  }

  const uint16_t entryCount = cachedRecordCount + 1;
  virtualOffsets.clear();
  virtualOffsets.reserve(entryCount);

  for (uint16_t i = 0; i < entryCount; i++) {
    uint32_t val = 0;
    if (f.read(&val, 4) != 4) {
      f.close();
      virtualOffsets.clear();
      return false;
    }
    virtualOffsets.push_back(val);
  }

  f.close();
  virtualTextSize = virtualOffsets.back();
  LOG_DBG("MOBI", "Loaded virtual offset table from cache: %u records, %u virtual bytes",
          textRecordCount, virtualTextSize);
  return true;
}

bool Mobi::saveVirtualOffsetTable() const {
  setupCacheDir();
  const std::string cachefile = cachePath + "/voffsets.bin";
  FsFile f;
  if (!Storage.openFileForWrite("MOBI", cachefile, f)) {
    LOG_ERR("MOBI", "Failed to write virtual offset cache");
    return false;
  }

  f.write(reinterpret_cast<const uint8_t*>(&VOFFSET_MAGIC), 4);
  f.write(&VOFFSET_VERSION, 1);
  f.write(reinterpret_cast<const uint8_t*>(&fileSize), 4);
  f.write(reinterpret_cast<const uint8_t*>(&textRecordCount), 2);
  f.write(reinterpret_cast<const uint8_t*>(&compressionType), 2);
  f.write(reinterpret_cast<const uint8_t*>(&extraDataFlags), 2);

  for (uint32_t v : virtualOffsets) {
    f.write(reinterpret_cast<const uint8_t*>(&v), 4);
  }

  f.close();
  return true;
}

// ---------------------------------------------------------------------------
// Per-record I/O
// ---------------------------------------------------------------------------

size_t Mobi::readRawRecord(FsFile& file, uint16_t recIdx, uint8_t* buf, size_t bufSize) const {
  // Text records are at PalmDB record indices 1 .. textRecordCount (0-based in recordFileOffsets)
  const uint16_t palmRecIdx = recIdx + 1;

  if (palmRecIdx >= static_cast<uint16_t>(recordFileOffsets.size())) {
    LOG_ERR("MOBI", "Record index %u out of range", palmRecIdx);
    return 0;
  }

  const uint32_t recStart = recordFileOffsets[palmRecIdx];
  const uint32_t recEnd = (palmRecIdx + 1 < static_cast<uint16_t>(recordFileOffsets.size()))
                              ? recordFileOffsets[palmRecIdx + 1]
                              : fileSize;

  if (recEnd <= recStart) return 0;

  size_t rawSize = static_cast<size_t>(recEnd - recStart);
  if (rawSize > bufSize) {
    rawSize = bufSize;  // Clamp — should not happen with a correctly sized buffer
  }

  if (!file.seek(recStart) || file.read(buf, rawSize) != rawSize) {
    LOG_ERR("MOBI", "Failed to read text record %u", recIdx);
    return 0;
  }

  return stripTrailingBytes(buf, rawSize);
}

size_t Mobi::stripTrailingBytes(const uint8_t* buf, size_t rawSize) const {
  if (extraDataFlags == 0 || rawSize == 0) return rawSize;

  size_t trailing = 0;

  // Process trailing-entry bits (bits 1–14): each set bit indicates one trailing entry
  // whose byte size is encoded as a 1-byte count at the current end of the record.
  // We use a simple 1-byte VLQ (handles the common case; multi-byte is rare).
  for (int bit = 1; bit <= 14; bit++) {
    if (!(extraDataFlags & (1 << bit))) continue;
    if (rawSize <= trailing) break;
    const uint8_t lastByte = buf[rawSize - 1 - trailing];
    trailing += (lastByte & 0x3F) + 1;
  }

  // Bit 0: multibyte character overlap — strip N+1 bytes where N = last byte & 0x3
  if (extraDataFlags & 0x0001) {
    if (rawSize > trailing) {
      trailing += (buf[rawSize - 1 - trailing] & 0x3) + 1;
    }
  }

  if (trailing >= rawSize) return 0;
  return rawSize - trailing;
}

// ---------------------------------------------------------------------------
// PalmDOC decompressor
// ---------------------------------------------------------------------------

size_t Mobi::decompressPalmDoc(const uint8_t* in, size_t inLen, uint8_t* out, size_t outMax) {
  size_t i = 0;  // Input position
  size_t o = 0;  // Output position

  while (i < inLen && o < outMax) {
    const uint8_t c = in[i++];

    if (c == 0x00) {
      // Literal null byte
      out[o++] = 0x00;
    } else if (c <= 0x08) {
      // Next c bytes are raw literals
      const size_t count = c;
      if (i + count > inLen || o + count > outMax) break;
      memcpy(out + o, in + i, count);
      o += count;
      i += count;
    } else if (c <= 0x7F) {
      // Single literal byte (printable ASCII and some control chars)
      out[o++] = c;
    } else if (c <= 0xBF) {
      // Two-byte back-reference
      if (i >= inLen) break;
      const uint8_t next = in[i++];
      const uint16_t pair = (static_cast<uint16_t>(c) << 8) | next;
      const uint16_t dist = (pair >> 3) & 0x07FF;
      const uint8_t len = (pair & 0x07) + 3;

      if (dist == 0 || dist > o) break;  // Invalid back-reference

      size_t src = o - dist;
      for (uint8_t k = 0; k < len && o < outMax; k++) {
        out[o++] = out[src++];
      }
    } else {
      // 0xC0–0xFF: space + printable character
      if (o + 2 > outMax) break;
      out[o++] = ' ';
      out[o++] = c ^ 0x80;
    }
  }

  return o;
}

// ---------------------------------------------------------------------------
// HTML tag stripper
// ---------------------------------------------------------------------------

size_t Mobi::stripHtml(const uint8_t* in, size_t inLen, uint8_t* out, size_t outMax) {
  size_t i = 0;
  size_t o = 0;

  while (i < inLen && o < outMax) {
    if (in[i] == '<') {
      // Parse tag name
      i++;  // Skip '<'
      while (i < inLen && (in[i] == ' ' || in[i] == '\t')) i++;  // Skip whitespace

      bool isClosing = false;
      if (i < inLen && in[i] == '/') {
        isClosing = true;
        i++;
      }

      char tagName[16] = {};
      size_t tn = 0;
      while (i < inLen && in[i] != '>' && in[i] != ' ' && in[i] != '/' && in[i] != '\t' && tn < 15) {
        tagName[tn++] = static_cast<char>(tolower(static_cast<unsigned char>(in[i])));
        i++;
      }
      tagName[tn] = '\0';

      // Skip to end of tag
      while (i < inLen && in[i] != '>') i++;
      if (i < inLen) i++;  // Skip '>'

      // Block-level tags produce a newline to preserve paragraph structure.
      // Only emit a newline for opening tags (or self-closing <br>), and avoid doubles.
      const bool isBr = (strcmp(tagName, "br") == 0);
      const bool isBlock =
          isBr || (!isClosing &&
                   (strcmp(tagName, "p") == 0 || strcmp(tagName, "div") == 0 ||
                    strcmp(tagName, "h1") == 0 || strcmp(tagName, "h2") == 0 ||
                    strcmp(tagName, "h3") == 0 || strcmp(tagName, "h4") == 0 ||
                    strcmp(tagName, "h5") == 0 || strcmp(tagName, "h6") == 0 ||
                    strcmp(tagName, "li") == 0 || strcmp(tagName, "tr") == 0 ||
                    strcmp(tagName, "hr") == 0));

      if (isBlock && o < outMax) {
        if (o == 0 || out[o - 1] != '\n') {
          out[o++] = '\n';
        }
      }

    } else if (in[i] == '&') {
      // HTML entity
      i++;  // Skip '&'
      char entity[12] = {};
      size_t en = 0;
      while (i < inLen && in[i] != ';' && in[i] != ' ' && en < 11) {
        entity[en++] = static_cast<char>(in[i++]);
      }
      if (i < inLen && in[i] == ';') i++;  // Skip ';'
      entity[en] = '\0';

      char decoded = 0;
      if (strcmp(entity, "amp") == 0)       decoded = '&';
      else if (strcmp(entity, "lt") == 0)   decoded = '<';
      else if (strcmp(entity, "gt") == 0)   decoded = '>';
      else if (strcmp(entity, "quot") == 0) decoded = '"';
      else if (strcmp(entity, "apos") == 0) decoded = '\'';
      else if (strcmp(entity, "nbsp") == 0) decoded = ' ';
      else if (strcmp(entity, "shy") == 0)  decoded = 0;  // Soft hyphen — skip
      else if (entity[0] == '#') {
        // Numeric character reference
        int codepoint = 0;
        if (entity[1] == 'x' || entity[1] == 'X') {
          codepoint = static_cast<int>(strtol(entity + 2, nullptr, 16));
        } else {
          codepoint = atoi(entity + 1);
        }
        if (codepoint > 0 && codepoint < 128) {
          decoded = static_cast<char>(codepoint);
        }
        // Non-ASCII codepoints skipped (would need UTF-8 encoding)
      }

      if (decoded != 0 && o < outMax) {
        out[o++] = static_cast<uint8_t>(decoded);
      }

    } else {
      out[o++] = in[i++];
    }
  }

  return o;
}
