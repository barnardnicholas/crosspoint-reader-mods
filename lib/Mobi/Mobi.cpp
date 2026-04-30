#include "Mobi.h"

#include <FsHelpers.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

namespace {

// Cache file identifiers
constexpr uint32_t VOFFSET_MAGIC = 0x4D424F49;  // "MBOI"
constexpr uint8_t VOFFSET_VERSION = 2;

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
constexpr uint32_t EXTH_KF8_BOUNDARY = 121;

// Compression types
constexpr uint16_t COMPRESSION_NONE = 1;
constexpr uint16_t COMPRESSION_PALMDOC = 2;
constexpr uint16_t COMPRESSION_HUFFMAN = 17480;

// MOBI type value indicating KF8 format
constexpr uint32_t MOBI_TYPE_KF8 = 248;

// Offset of MOBI type field within rec0 (MOBI magic at 16, type at +8 = 24)
constexpr size_t REC0_MOBI_TYPE_OFFSET = 24;

}  // namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

Mobi::Mobi(std::string path, std::string cacheBasePath)
    : filepath(std::move(path)), cacheBasePath(std::move(cacheBasePath)) {
  const size_t hash = std::hash<std::string>{}(filepath);
  cachePath = this->cacheBasePath + "/mobi_" + std::to_string(hash);
}

Mobi::~Mobi() {
  for (uint8_t i = 0; i < cdicTable.recordCount; i++) {
    free(cdicTable.records[i]);
    cdicTable.records[i] = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

bool Mobi::isDrmLocked(const char* path) {
  FsFile file;
  if (!Storage.openFileForRead("MOBI", path, file)) return false;

  // Read PalmDB header (78 bytes) to get record 0 offset
  uint8_t hdr[PALMDB_HEADER_SIZE];
  if (file.read(hdr, sizeof(hdr)) != sizeof(hdr)) {
    file.close();
    return false;
  }

  const uint16_t numRecords = readU16BE(hdr + PALMDB_RECORD_COUNT_OFFSET);
  if (numRecords < 2) {
    file.close();
    return false;
  }

  // Read first record list entry (8 bytes) to get record 0 file offset
  uint8_t recEntry[PALMDB_RECORD_ENTRY_SIZE];
  if (file.read(recEntry, sizeof(recEntry)) != sizeof(recEntry)) {
    file.close();
    return false;
  }

  const uint32_t rec0Start = readU32BE(recEntry);

  // Read up to 100 bytes of Record 0
  uint8_t rec0[100];
  if (!file.seek(rec0Start)) {
    file.close();
    return false;
  }
  const size_t readLen = file.read(rec0, sizeof(rec0));
  file.close();

  if (readLen < 14) return false;  // Need at least offset 12+2 for encryptionType

  // PalmDOC header: encryptionType at offset 12 (uint16 BE)
  const uint16_t encryptionType = readU16BE(rec0 + 12);
  return encryptionType == 2;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool Mobi::load() {
  if (loaded) return true;
  if (!loadHeader()) return false;
  if (!detectFormat()) return false;

  if (mobiVariant == MobiVariant::KF8 || compressionType == COMPRESSION_HUFFMAN) {
    if (!loadHuffCdic()) return false;
  }

  // Try loading cached virtual offset table first; build if absent.
  if (!loadVirtualOffsetTable()) {
    if (!buildVirtualOffsetTable()) {
      LOG_ERR("MOBI", "Failed to build virtual offset table");
      return false;
    }
    saveVirtualOffsetTable();
  }

  if (mobiVariant == MobiVariant::KF8) {
    parseToc();  // Non-fatal: failure means no chapters
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

  headerLoaded = true;
  LOG_DBG("MOBI", "Header loaded: title='%s' author='%s' compression=%u records=%u", title.c_str(),
          author.c_str(), compressionType, textRecordCount);
  return true;
}

bool Mobi::detectFormat() {
  // mobiTypeField and kf8SectionRecord were populated by parseMobiHeaders().
  // kf8SectionRecord == 0xFFFFFFFF means no EXTH 121 was found.

  if (mobiTypeField == MOBI_TYPE_KF8 || kf8SectionRecord != 0xFFFFFFFF) {
    mobiVariant = MobiVariant::KF8;
    LOG_DBG("MOBI", "KF8 format detected: mobiType=%u kf8SectionRecord=%u", mobiTypeField, kf8SectionRecord);
    return true;
  }

  mobiVariant = MobiVariant::MOBI7;
  return true;
}

std::string Mobi::getTitle() const {
  if (!title.empty()) return title;

  // Fall back to filename without extension
  size_t lastSlash = filepath.find_last_of('/');
  std::string filename = (lastSlash != std::string::npos) ? filepath.substr(lastSlash + 1) : filepath;
  if (FsHelpers::hasMobiExtension(filename)) {
    const size_t dotPos = filename.rfind('.');
    if (dotPos != std::string::npos) {
      filename = filename.substr(0, dotPos);
    }
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

    size_t decompLen = 0;
    if (compressionType == COMPRESSION_HUFFMAN) {
      decompLen = decompressHuffCdic(rawBuf, rawSize, decompBuf, decompBufSize);
    } else if (compressionType == COMPRESSION_PALMDOC) {
      decompLen = decompressPalmDoc(rawBuf, rawSize, decompBuf, decompBufSize);
    } else {
      if (rawSize <= decompBufSize) {
        memcpy(decompBuf, rawBuf, rawSize);
        decompLen = rawSize;
      }
    }

    // Strip HTML in-place (output is <= input)
    size_t strippedLen = stripHtml(decompBuf, decompLen, decompBuf, decompLen);

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

  // DRM check: encryptionType at PalmDOC header offset 12 (uint16 BE)
  if (rec0Size >= 14) {
    const uint16_t encryptionType = readU16BE(buf + 12);
    if (encryptionType == 2) {
      free(buf);
      lastError = MobiError::DrmProtected;
      LOG_ERR("MOBI", "DRM-encrypted file — cannot open");
      return false;
    }
  }

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

  // Cache MOBI type field (rec0 offset 24 = MOBI magic offset 16 + type offset 8)
  if (rec0Size >= REC0_MOBI_TYPE_OFFSET + 4) {
    mobiTypeField = readU32BE(buf + REC0_MOBI_TYPE_OFFSET);
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
          } else if (recType == EXTH_KF8_BOUNDARY && dataLen == 4) {
            uint32_t boundary;
            memcpy(&boundary, buf + dataStart, 4);
            kf8SectionRecord = __builtin_bswap32(boundary);
          }
        }

        pos += recLen;
      }
    }
  }

  free(buf);

  // If KF8 markers found, re-parse the KF8 section Record 0 to get KF8-specific header fields.
  // The KF8 section has its own compressionType (17480), textRecordCount, maxRecordSize,
  // extraDataFlags — different from the MOBI7 values just parsed.
  const bool isKf8 = (mobiTypeField == MOBI_TYPE_KF8 || kf8SectionRecord != 0xFFFFFFFF);
  if (isKf8 && kf8SectionRecord != 0xFFFFFFFF &&
      kf8SectionRecord < static_cast<uint32_t>(recordFileOffsets.size())) {
    const uint32_t kf8Rec0Start = recordFileOffsets[kf8SectionRecord];
    const uint32_t kf8Rec0End =
        (kf8SectionRecord + 1 < static_cast<uint32_t>(recordFileOffsets.size()))
            ? recordFileOffsets[kf8SectionRecord + 1]
            : fileSize;
    const uint32_t kf8Rec0Size = std::min<uint32_t>(kf8Rec0End - kf8Rec0Start, REC0_READ_SIZE);

    auto* kf8Buf = static_cast<uint8_t*>(malloc(kf8Rec0Size));
    if (kf8Buf) {
      if (file.seek(kf8Rec0Start) && file.read(kf8Buf, kf8Rec0Size) == kf8Rec0Size) {
        // Override with KF8 section header values
        compressionType = readU16BE(kf8Buf + REC0_COMPRESSION_OFFSET);
        rawTextLength = readU32BE(kf8Buf + REC0_TEXT_LENGTH_OFFSET);
        textRecordCount = readU16BE(kf8Buf + REC0_RECORD_COUNT_OFFSET);
        maxRecordSize = readU16BE(kf8Buf + REC0_RECORD_SIZE_OFFSET);
        if (maxRecordSize == 0) maxRecordSize = 4096;
        if (kf8Rec0Size >= REC0_EXTRA_DATA_FLAGS_OFFSET + 2) {
          extraDataFlags = readU16BE(kf8Buf + REC0_EXTRA_DATA_FLAGS_OFFSET);
        }
        kf8FirstTextRecord = kf8SectionRecord + 1;
        LOG_DBG("MOBI", "KF8 section: compression=%u records=%u firstTextRec=%u",
                compressionType, textRecordCount, kf8FirstTextRecord);
      } else {
        LOG_ERR("MOBI", "Failed to read KF8 section Record 0");
      }
      free(kf8Buf);
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// Huffman/CDIC decompressor
// ---------------------------------------------------------------------------

bool Mobi::loadHuffCdic() {
  // KF8: reads huffRecordOffset/cdicCount from the type-248 section header (fixed offsets).
  // MOBI7: scans non-text records for "HUFF" magic (field position varies by file version).

  uint32_t headerRecIdx;
  if (mobiVariant == MobiVariant::KF8) {
    if (kf8SectionRecord >= static_cast<uint32_t>(recordFileOffsets.size())) {
      LOG_ERR("MOBI", "KF8 section record %u out of range", kf8SectionRecord);
      return false;
    }
    headerRecIdx = kf8SectionRecord;
  } else {
    headerRecIdx = 0;  // MOBI7: huff fields live in Record 0
  }

  FsFile file;
  if (!Storage.openFileForRead("MOBI", filepath, file)) return false;

  const uint32_t hdrRecStart = recordFileOffsets[headerRecIdx];
  const uint32_t hdrRecEnd =
      (headerRecIdx + 1 < static_cast<uint32_t>(recordFileOffsets.size()))
          ? recordFileOffsets[headerRecIdx + 1]
          : fileSize;
  const uint32_t hdrRecSize = std::min<uint32_t>(hdrRecEnd - hdrRecStart, REC0_READ_SIZE);

  auto* hdrBuf = static_cast<uint8_t*>(malloc(hdrRecSize));
  if (!hdrBuf) {
    file.close();
    LOG_ERR("MOBI", "malloc failed for huff header");
    return false;
  }

  if (!file.seek(hdrRecStart) || file.read(hdrBuf, hdrRecSize) != hdrRecSize) {
    free(hdrBuf);
    file.close();
    LOG_ERR("MOBI", "Failed to read huff header record %u", headerRecIdx);
    return false;
  }

  if (hdrRecSize < REC0_MOBI_MAGIC_OFFSET + 4 || hdrBuf[REC0_MOBI_MAGIC_OFFSET] != 'M' ||
      hdrBuf[REC0_MOBI_MAGIC_OFFSET + 1] != 'O' || hdrBuf[REC0_MOBI_MAGIC_OFFSET + 2] != 'B' ||
      hdrBuf[REC0_MOBI_MAGIC_OFFSET + 3] != 'I') {
    free(hdrBuf);
    file.close();
    LOG_ERR("MOBI", "Huff header record missing MOBI magic");
    return false;
  }

  uint32_t huffRecordOffset = 0;
  uint32_t cdicCount = 0;

  if (mobiVariant == MobiVariant::KF8) {
    // KF8: read pointers from the type-248 section header at fixed offsets.
    constexpr size_t HUFF_REC_OFFSET   = REC0_MOBI_MAGIC_OFFSET + 104;  // 120
    constexpr size_t HUFF_COUNT_OFFSET = REC0_MOBI_MAGIC_OFFSET + 108;  // 124
    if (hdrRecSize < HUFF_COUNT_OFFSET + 4) {
      free(hdrBuf);
      file.close();
      LOG_ERR("MOBI", "KF8 header too small for huff fields");
      return false;
    }
    huffRecordOffset = readU32BE(hdrBuf + HUFF_REC_OFFSET);
    cdicCount        = readU32BE(hdrBuf + HUFF_COUNT_OFFSET);
    free(hdrBuf);
    LOG_DBG("MOBI", "KF8 huff record at PalmDB index %u, %u CDIC records", huffRecordOffset, cdicCount);
  } else {
    // MOBI7: the huffrec field position varies by file version.
    // Scan records after the text records for "HUFF" magic, then count "CDIC" records.
    free(hdrBuf);
    const uint32_t firstNonText = kf8FirstTextRecord + textRecordCount;
    bool found = false;
    for (uint32_t i = firstNonText; i < static_cast<uint32_t>(recordFileOffsets.size()); i++) {
      const uint32_t rStart = recordFileOffsets[i];
      uint8_t magic[4] = {};
      if (!file.seek(rStart) || file.read(magic, 4) != 4) continue;
      if (magic[0] == 'H' && magic[1] == 'U' && magic[2] == 'F' && magic[3] == 'F') {
        huffRecordOffset = i;
        for (uint32_t j = i + 1; j < static_cast<uint32_t>(recordFileOffsets.size()); j++) {
          uint8_t cm[4] = {};
          if (!file.seek(recordFileOffsets[j]) || file.read(cm, 4) != 4) break;
          if (cm[0] == 'C' && cm[1] == 'D' && cm[2] == 'I' && cm[3] == 'C') {
            cdicCount++;
          } else {
            break;
          }
        }
        found = true;
        break;
      }
    }
    if (!found) {
      file.close();
      LOG_ERR("MOBI", "MOBI7 HUFF record not found in %u non-text records",
              static_cast<uint32_t>(recordFileOffsets.size()) - firstNonText);
      return false;
    }
    LOG_DBG("MOBI", "MOBI7 huff record at PalmDB index %u, %u CDIC records", huffRecordOffset, cdicCount);
  }

  // Cap check BEFORE any malloc
  if (cdicCount > MAX_CDIC_RECORDS) {
    file.close();
    lastError = MobiError::CdicCapExceeded;
    LOG_ERR("MOBI", "CDIC record count %u exceeds cap %u", cdicCount, MAX_CDIC_RECORDS);
    return false;
  }

  // Read HUFF record: it's at PalmDB record index huffRecordOffset (absolute record index)
  if (huffRecordOffset >= static_cast<uint32_t>(recordFileOffsets.size())) {
    file.close();
    LOG_ERR("MOBI", "HUFF record index %u out of range", huffRecordOffset);
    return false;
  }

  const uint32_t huffStart = recordFileOffsets[huffRecordOffset];
  const uint32_t huffEnd =
      (huffRecordOffset + 1 < static_cast<uint32_t>(recordFileOffsets.size()))
          ? recordFileOffsets[huffRecordOffset + 1]
          : fileSize;

  // HUFF record minimum size: 4 magic + 4 hdrlen + 4 dict1off + 4 dict2off + 4 d1len + 4 d2len
  // + 256*4 dict1 + 64*4 dict2 = 24 + 1024 + 256 = 1304 bytes minimum
  constexpr size_t HUFF_MIN_SIZE = 24 + 256 * 4 + 64 * 4;
  if (huffEnd <= huffStart || huffEnd - huffStart < HUFF_MIN_SIZE) {
    file.close();
    LOG_ERR("MOBI", "HUFF record too small");
    return false;
  }

  auto* huffBuf = static_cast<uint8_t*>(malloc(huffEnd - huffStart));
  if (!huffBuf) {
    file.close();
    LOG_ERR("MOBI", "malloc failed for HUFF record");
    return false;
  }

  if (!file.seek(huffStart) || file.read(huffBuf, huffEnd - huffStart) != huffEnd - huffStart) {
    free(huffBuf);
    file.close();
    LOG_ERR("MOBI", "Failed to read HUFF record");
    return false;
  }

  // Validate HUFF magic
  if (huffBuf[0] != 'H' || huffBuf[1] != 'U' || huffBuf[2] != 'F' || huffBuf[3] != 'F') {
    free(huffBuf);
    file.close();
    LOG_ERR("MOBI", "HUFF record missing magic");
    return false;
  }

  // Parse dict1 and dict2 offsets from HUFF header
  const uint32_t dict1Off = readU32BE(huffBuf + 8);
  const uint32_t dict2Off = readU32BE(huffBuf + 12);
  const uint32_t huffRecLen = static_cast<uint32_t>(huffEnd - huffStart);

  if (dict1Off + 256 * 4 > huffRecLen || dict2Off + 64 * 4 > huffRecLen) {
    free(huffBuf);
    file.close();
    LOG_ERR("MOBI", "HUFF dict offsets out of range");
    return false;
  }

  // Load dict1 (256 × uint32 BE) and dict2 (64 × uint32 BE) via readU32BE to avoid alignment faults
  for (int i = 0; i < 256; i++) {
    huffTable.dict1[i] = readU32BE(huffBuf + dict1Off + i * 4);
  }
  for (int i = 0; i < 64; i++) {
    huffTable.dict2[i] = readU32BE(huffBuf + dict2Off + i * 4);
  }
  free(huffBuf);

  // Load CDIC records (they follow the HUFF record sequentially)
  for (uint32_t i = 0; i < cdicCount; i++) {
    const uint32_t cdicPalmIdx = huffRecordOffset + 1 + i;
    if (cdicPalmIdx >= static_cast<uint32_t>(recordFileOffsets.size())) {
      LOG_ERR("MOBI", "CDIC record %u index %u out of range", i, cdicPalmIdx);
      // Free already-allocated blocks
      for (uint8_t j = 0; j < cdicTable.recordCount; j++) {
        free(cdicTable.records[j]);
        cdicTable.records[j] = nullptr;
      }
      cdicTable.recordCount = 0;
      file.close();
      return false;
    }

    const uint32_t cdicStart = recordFileOffsets[cdicPalmIdx];
    const uint32_t cdicEnd =
        (cdicPalmIdx + 1 < static_cast<uint32_t>(recordFileOffsets.size()))
            ? recordFileOffsets[cdicPalmIdx + 1]
            : fileSize;
    const uint32_t cdicLen = std::min<uint32_t>(cdicEnd - cdicStart, CDIC_RECORD_SIZE);

    cdicTable.records[i] = static_cast<uint8_t*>(malloc(CDIC_RECORD_SIZE));
    if (!cdicTable.records[i]) {
      LOG_ERR("MOBI", "malloc failed for CDIC record %u", i);
      for (uint8_t j = 0; j < cdicTable.recordCount; j++) {
        free(cdicTable.records[j]);
        cdicTable.records[j] = nullptr;
      }
      cdicTable.recordCount = 0;
      file.close();
      return false;
    }
    memset(cdicTable.records[i], 0, CDIC_RECORD_SIZE);

    if (!file.seek(cdicStart) || file.read(cdicTable.records[i], cdicLen) != cdicLen) {
      LOG_ERR("MOBI", "Failed to read CDIC record %u", i);
      free(cdicTable.records[i]);
      cdicTable.records[i] = nullptr;
      for (uint8_t j = 0; j < cdicTable.recordCount; j++) {
        free(cdicTable.records[j]);
        cdicTable.records[j] = nullptr;
      }
      cdicTable.recordCount = 0;
      file.close();
      return false;
    }

    // Parse phrasesPerRecord from first CDIC record header (offset 8, uint32 BE)
    if (i == 0) {
      if (cdicLen >= 12) {
        cdicTable.phrasesPerRecord = static_cast<uint16_t>(readU32BE(cdicTable.records[0] + 8));
      }
    }

    cdicTable.recordCount = static_cast<uint8_t>(i + 1);
  }

  file.close();
  huffCdicLoaded = true;
  LOG_DBG("MOBI", "Loaded HUFF + %u CDIC records, phrasesPerRecord=%u", cdicTable.recordCount,
          cdicTable.phrasesPerRecord);
  return true;
}

size_t Mobi::decompressHuffCdic(const uint8_t* in, size_t inLen, uint8_t* out, size_t outMax) const {
  if (!huffCdicLoaded || cdicTable.phrasesPerRecord == 0) return 0;

  // Iterative Huffman decode with explicit work stack to avoid recursion.
  // Each work item is a phrase fragment that itself may need CDIC expansion.
  struct WorkItem {
    const uint8_t* data;
    uint16_t len;
    bool compressed;
  };
  WorkItem stack[32];
  int stackTop = 0;

  // Push the initial compressed input as a compressed work item
  if (stackTop >= 32) return 0;
  stack[stackTop++] = {in, static_cast<uint16_t>(inLen > 0xFFFF ? 0xFFFF : inLen), true};

  size_t outPos = 0;

  while (stackTop > 0 && outPos < outMax) {
    WorkItem item = stack[--stackTop];

    if (!item.compressed) {
      // Plain bytes — copy directly to output
      const size_t toCopy = std::min<size_t>(item.len, outMax - outPos);
      memcpy(out + outPos, item.data, toCopy);
      outPos += toCopy;
      continue;
    }

    // Huffman decode this compressed fragment
    const uint8_t* src = item.data;
    const size_t srcLen = item.len;
    size_t srcBit = 0;  // Current bit position within src

    while (srcBit < srcLen * 8 && outPos < outMax) {
      const size_t bOff = srcBit >> 3;
      if (bOff >= srcLen) break;

      // Extract 8 bits starting at srcBit (handles non-byte-aligned positions).
      // Using src[bOff] directly is wrong when srcBit % 8 != 0 — it re-reads
      // already-consumed bits, producing invalid dict1 lookups.
      const size_t shift = srcBit & 7;
      const uint8_t firstByte = (shift == 0)
          ? src[bOff]
          : static_cast<uint8_t>((src[bOff] << shift) |
                                  (bOff + 1 < srcLen ? src[bOff + 1] >> (8 - shift) : 0));

      const uint32_t d1 = huffTable.dict1[firstByte];

      // dict1 entry: bits 31-27 = codelen (5 bits), bit 26 = term flag, bits 23-0 = phrase/maxcode
      uint8_t codeLen = static_cast<uint8_t>((d1 >> 27) & 0x1F);
      const bool term = (d1 >> 26) & 1;

      if (codeLen == 0) break;

      uint32_t phraseIdx = 0;
      if (term) {
        phraseIdx = d1 & 0x00FFFFFF;
      } else {
        // Non-terminal: build 32-bit code from srcBit and search dict2 for actual
        // code length. Must use 32 bits (not codeLen bits) — dict1 codeLen for
        // non-terminal entries is only a hint, not the exact length.
        uint32_t code = 0;
        for (uint8_t b = 0; b < 32; b++) {
          const size_t bitPos = srcBit + b;
          const size_t bp = bitPos >> 3;
          if (bp < srcLen) {
            code |= static_cast<uint32_t>((src[bp] >> (7 - (bitPos & 7))) & 1) << (31 - b);
          }
        }

        for (uint8_t cl = 1; cl <= 32; cl++) {
          if ((cl - 1) * 2 + 1 >= 64) break;
          const uint32_t minC = huffTable.dict2[(cl - 1) * 2];
          const uint32_t maxC = huffTable.dict2[(cl - 1) * 2 + 1];
          if (code >= minC && code <= maxC) {
            codeLen = cl;
            phraseIdx = (maxC >> (32 - cl)) - (code >> (32 - cl));
            break;
          }
        }
      }

      srcBit += codeLen;

      // Look up phrase in CDIC
      const uint16_t ppr = cdicTable.phrasesPerRecord;
      if (ppr == 0) break;
      const uint32_t recIdx = phraseIdx / ppr;
      const uint32_t posInRec = phraseIdx % ppr;

      if (recIdx >= cdicTable.recordCount) {
        LOG_ERR("MOBI", "CDIC record index %u out of range", recIdx);
        break;
      }

      const uint8_t* cdicRec = cdicTable.records[recIdx];

      // CDIC offset table: starts at header end (offset 16 in CDIC record)
      // Each entry is a uint16 BE offset from the start of phrase data
      // codeLength field is at CDIC header offset 12 (uint32 BE)
      const uint32_t codeLength = readU32BE(cdicRec + 12);

      // Bounds check — CVE GHSA-5mwx-65x7-cffp: must check 2×(1<<codeLength) entries
      const uint32_t maxEntries = 1u << codeLength;
      if (posInRec >= maxEntries) {
        LOG_ERR("MOBI", "CDIC bounds check failed: pos %u >= maxEntries %u", posInRec, maxEntries);
        break;
      }

      // CDIC header is 16 bytes; offset table follows immediately
      constexpr uint32_t CDIC_HDR_SIZE = 16;
      const uint32_t offsetTableEntry = CDIC_HDR_SIZE + posInRec * 2;
      if (offsetTableEntry + 2 > CDIC_RECORD_SIZE) break;

      uint16_t phraseOffset;
      memcpy(&phraseOffset, cdicRec + offsetTableEntry, 2);
      // Big-endian
      phraseOffset = static_cast<uint16_t>((phraseOffset >> 8) | (phraseOffset << 8));

      const bool phraseCompressed = (phraseOffset & 0x8000) == 0;
      phraseOffset &= 0x7FFF;

      // Phrase length: difference between this entry's offset and next
      uint16_t nextOffset;
      if (posInRec + 1 < maxEntries && offsetTableEntry + 4 <= CDIC_RECORD_SIZE) {
        memcpy(&nextOffset, cdicRec + offsetTableEntry + 2, 2);
        nextOffset = static_cast<uint16_t>((nextOffset >> 8) | (nextOffset << 8));
        nextOffset &= 0x7FFF;
      } else {
        // Last phrase — extends to end of record (use a safe upper bound)
        nextOffset = static_cast<uint16_t>(CDIC_RECORD_SIZE - CDIC_HDR_SIZE);
      }

      const uint32_t phraseDataStart = CDIC_HDR_SIZE + maxEntries * 2 + phraseOffset;
      const uint32_t phraseDataEnd = CDIC_HDR_SIZE + maxEntries * 2 + nextOffset;

      if (phraseDataStart >= CDIC_RECORD_SIZE || phraseDataEnd > CDIC_RECORD_SIZE ||
          phraseDataEnd <= phraseDataStart) {
        break;
      }

      const uint16_t phraseLen = static_cast<uint16_t>(phraseDataEnd - phraseDataStart);

      if (phraseCompressed) {
        // Push for recursive expansion — but only if stack has room
        if (stackTop < 32) {
          stack[stackTop++] = {cdicRec + phraseDataStart, phraseLen, true};
        }
      } else {
        const size_t toCopy = std::min<size_t>(phraseLen, outMax - outPos);
        memcpy(out + outPos, cdicRec + phraseDataStart, toCopy);
        outPos += toCopy;
      }
    }
  }

  return outPos;
}

// ---------------------------------------------------------------------------
// TOC / chapter navigation (KF8 NCX INDX)
// ---------------------------------------------------------------------------

bool Mobi::parseToc() {
  chapters.clear();

  // NCX record index is at KF8 MOBI header offset 160 (relative to MOBI magic in KF8 rec0).
  // Absolute rec0 offset = 16 (MOBI magic offset) + 160 = 176.
  constexpr size_t KF8_NCX_INDEX_OFFSET = 176;

  if (kf8SectionRecord >= static_cast<uint32_t>(recordFileOffsets.size())) {
    return true;  // No KF8 section
  }

  // Read enough of the KF8 rec0 to get the NCX field
  const uint32_t kf8Rec0Start = recordFileOffsets[kf8SectionRecord];
  const uint32_t kf8Rec0Needed = kf8Rec0Start + KF8_NCX_INDEX_OFFSET + 4;
  if (kf8Rec0Needed > fileSize) return true;

  FsFile file;
  if (!Storage.openFileForRead("MOBI", filepath, file)) return true;

  constexpr size_t HDR_BUF_SIZE = KF8_NCX_INDEX_OFFSET + 4;
  uint8_t hdrBuf[HDR_BUF_SIZE];
  if (!file.seek(kf8Rec0Start) || file.read(hdrBuf, HDR_BUF_SIZE) != HDR_BUF_SIZE) {
    file.close();
    return true;
  }

  const uint32_t ncxRelIdx = readU32BE(hdrBuf + KF8_NCX_INDEX_OFFSET);
  if (ncxRelIdx == 0xFFFFFFFF) {
    file.close();
    return true;  // No NCX
  }

  // The NCX record index is relative to KF8 Record 0's position in the PalmDB record list
  const uint32_t ncxPalmIdx = kf8SectionRecord + ncxRelIdx;
  if (ncxPalmIdx >= static_cast<uint32_t>(recordFileOffsets.size())) {
    file.close();
    return true;
  }

  const uint32_t indxStart = recordFileOffsets[ncxPalmIdx];
  const uint32_t indxEnd =
      (ncxPalmIdx + 1 < static_cast<uint32_t>(recordFileOffsets.size()))
          ? recordFileOffsets[ncxPalmIdx + 1]
          : fileSize;
  const uint32_t indxLen = indxEnd - indxStart;

  if (indxLen < 48) {
    file.close();
    return true;
  }

  auto* indxBuf = static_cast<uint8_t*>(malloc(indxLen));
  if (!indxBuf) {
    file.close();
    LOG_ERR("MOBI", "malloc failed for INDX record (%u bytes)", indxLen);
    return true;
  }

  if (!file.seek(indxStart) || file.read(indxBuf, indxLen) != indxLen) {
    free(indxBuf);
    file.close();
    LOG_ERR("MOBI", "Failed to read INDX record");
    return true;
  }
  file.close();

  // Validate INDX magic
  if (indxBuf[0] != 'I' || indxBuf[1] != 'N' || indxBuf[2] != 'D' || indxBuf[3] != 'X') {
    free(indxBuf);
    LOG_ERR("MOBI", "INDX record missing magic");
    return true;
  }

  // INDX header: offset of index entries section at byte 40 (uint32 BE)
  const uint32_t hdrLen = readU32BE(indxBuf + 4);
  const uint32_t entryCount = readU32BE(indxBuf + 24);
  const uint32_t entrySectionOffset = readU32BE(indxBuf + 40);

  if (entrySectionOffset >= indxLen || hdrLen >= indxLen) {
    free(indxBuf);
    return true;
  }

  chapters.reserve(entryCount > 512 ? 512 : entryCount);

  uint32_t pos = entrySectionOffset;
  uint32_t parsed = 0;

  while (pos < indxLen && parsed < entryCount && parsed < 512) {
    if (pos >= indxLen) break;

    // Label: 1-byte length prefix + UTF-8 bytes
    const uint8_t labelLen = indxBuf[pos++];
    if (pos + labelLen > indxLen) break;

    std::string label(reinterpret_cast<const char*>(indxBuf + pos), labelLen);
    pos += labelLen;

    if (pos >= indxLen) break;

    // Control byte: number of tags
    const uint8_t numControlBytes = indxBuf[pos++];

    // Skip control bytes to find tag data
    // For MVP: scan for tag ID 1 (text offset) in the tag data
    // Tags are variable-length VLQ-encoded values. We do a simplified parse:
    // skip numControlBytes bytes of bitmask, then read VLQ values.
    if (pos + numControlBytes > indxLen) break;

    // Read control bitmask bytes
    uint8_t controlBytes[8] = {};
    const uint8_t ctrlLen = numControlBytes < 8 ? numControlBytes : 8;
    memcpy(controlBytes, indxBuf + pos, ctrlLen);
    pos += numControlBytes;

    // Tag table in this INDX record starts at hdrLen; simplified: just scan VLQ values
    // For each bit set in controlBytes, there's a VLQ value in the tag data stream.
    // Tag ID 1 is the file offset tag — it's always the first tag value.
    uint32_t virtualOffset = 0;
    bool gotOffset = false;

    // Decode first VLQ value (tag ID 1 = text offset)
    // VLQ: each byte contributes 7 bits; high bit set = more bytes follow
    uint32_t vlqVal = 0;
    for (int vb = 0; vb < 4 && pos < indxLen; vb++) {
      const uint8_t b = indxBuf[pos++];
      vlqVal = (vlqVal << 7) | (b & 0x7F);
      if (!(b & 0x80)) {
        virtualOffset = vlqVal;
        gotOffset = true;
        break;
      }
    }

    if (gotOffset) {
      chapters.push_back({label, virtualOffset});
      parsed++;
    }

    // Skip remaining tag data for this entry (advance to next entry)
    // We skip remaining set bits in controlBytes
    for (uint8_t cb = 0; cb < ctrlLen && pos < indxLen; cb++) {
      for (int bit = 6; bit >= 0; bit--) {
        if (controlBytes[cb] & (1 << bit)) {
          // Skip one VLQ value
          for (int vb = 0; vb < 4 && pos < indxLen; vb++) {
            if (!(indxBuf[pos++] & 0x80)) break;
          }
        }
      }
    }
  }

  free(indxBuf);
  LOG_DBG("MOBI", "Parsed %zu chapters from INDX", chapters.size());
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
      size_t decompLen = 0;
      if (compressionType == COMPRESSION_HUFFMAN) {
        decompLen = decompressHuffCdic(rawBuf, rawSize, decompBuf, decompBufSize);
        if (decompLen == 0) {
          LOG_ERR("MOBI", "HuffCdic decompression failed on record %u", r);
        }
      } else if (compressionType == COMPRESSION_PALMDOC) {
        decompLen = decompressPalmDoc(rawBuf, rawSize, decompBuf, decompBufSize);
      } else {
        memcpy(decompBuf, rawBuf, rawSize);
        decompLen = rawSize;
      }
      strippedLen = stripHtml(decompBuf, decompLen, decompBuf, decompLen);
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
  // Text records start at kf8FirstTextRecord (1 for MOBI7, kf8SectionRecord+1 for KF8)
  const uint32_t palmRecIdx = kf8FirstTextRecord + recIdx;

  if (palmRecIdx >= static_cast<uint32_t>(recordFileOffsets.size())) {
    LOG_ERR("MOBI", "Record index %u out of range", palmRecIdx);
    return 0;
  }

  const uint32_t recStart = recordFileOffsets[palmRecIdx];
  const uint32_t recEnd = (palmRecIdx + 1 < static_cast<uint32_t>(recordFileOffsets.size()))
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
