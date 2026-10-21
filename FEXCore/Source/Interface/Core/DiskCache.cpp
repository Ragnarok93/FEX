// SPDX-License-Identifier: MIT

#define XXH_STATIC_LINKING_ONLY

#include "FEXCore/Config/Config.h"
#include "FEXCore/fextl/string.h"
#include "FEXHeaderUtils/Filesystem.h"
#include "FEXCore/Core/DiskCache.h"
#include "FEXCore/Core/DiskCacheFileMapper.h"
#include "FEXCore/Core/DiskCacheCapacity.h"
#include "FEXCore/Utils/LogManager.h"
#include "Interface/Context/Context.h"
#include "FEXCore/HLE/SyscallHandler.h"
#include "FEXCore/Utils/File.h"
#include "FEXCore/fextl/memory.h"
#include <cstdint>
#include <cstring>
#include <limits>
#include <algorithm>
#include <charconv>
#include <atomic>
#include <filesystem>
#include <iterator>
#include <utility>

namespace FEXCore {

namespace DiskCache {

  namespace MesaFOZ {

    enum { FOSSILIZE_COMPRESSION_NONE = 1, FOSSILIZE_COMPRESSION_DEFLATE = 2 };

    enum { FOSSILIZE_FORMAT_VERSION = 6, FOSSILIZE_FORMAT_MIN_COMPAT_VERSION = 5 };

#define FOZ_REF_MAGIC_SIZE 16

    static const uint8_t stream_reference_magic_and_version[FOZ_REF_MAGIC_SIZE] = {
      0x81, 'F', 'O', 'S', 'S', 'I', 'L', 'I', 'Z', 'E', 'D', 'B', 0, 0, 0, FOSSILIZE_FORMAT_VERSION, /* 4 bytes to use for versioning. */
    };

    struct __attribute__((packed)) mesa_index_db_file_entry {
      uint64_t hash;
      uint32_t size;
      uint64_t last_access_time;
      uint64_t cache_db_file_offset;
    };

  } // namespace MesaFOZ

  static FileMapperFunc FileMapper = nullptr;

  bool FOZFile::Open(const fextl::string& FOZFileName, bool ReadOnly) {
    FileName = FOZFileName;
    this->ReadOnly = ReadOnly;

    File::FileModes Modes = File::FileModes::READ;
    if (!ReadOnly) {
      Modes = Modes | File::FileModes::WRITE | File::FileModes::CREATE;
    }
    FD = fextl::make_unique<File::File>(FileName.c_str(), Modes, false);
    if (!FD->IsValid()) {
      FD.reset();
      return false;
    }

    bool Valid = false;
    bool TookLock = false;
    ssize_t Size = FD->Size();

    if (Size < FOZ_REF_MAGIC_SIZE && !ReadOnly) {
      if (!FD->Lock(OPEN_LOCK_TIMEOUT_MS)) {
        FD.reset();
        return false;
      }
      TookLock = true;
      // check size again in case someone else made it while we waited
      Size = FD->Size();
    }

    if (Size == 0 && !ReadOnly) {
      Valid = FD->PWrite(MesaFOZ::stream_reference_magic_and_version, FOZ_REF_MAGIC_SIZE, 0) == FOZ_REF_MAGIC_SIZE;
    } else {
      uint8_t magic[FOZ_REF_MAGIC_SIZE];
      if (FD->PRead(magic, FOZ_REF_MAGIC_SIZE, 0) == FOZ_REF_MAGIC_SIZE &&
          memcmp(magic, MesaFOZ::stream_reference_magic_and_version, FOZ_REF_MAGIC_SIZE - 1) == 0) {
        int version = magic[FOZ_REF_MAGIC_SIZE - 1];
        Valid = version <= MesaFOZ::FOSSILIZE_FORMAT_VERSION && version >= MesaFOZ::FOSSILIZE_FORMAT_MIN_COMPAT_VERSION;
      }
    }

    if (TookLock) {
      FD->Unlock();
    }

    if (!Valid) {
      FD.reset();
    }
    return Valid;
  }

  ssize_t FOZFile::Size() {
    return FD ? FD->Size() : -1;
  }

  bool FOZFile::ReadAll(fextl::vector<uint8_t>& Out) {
    ssize_t FileSize = Size();
    if (FileSize < FOZ_REF_MAGIC_SIZE) {
      return false;
    }
    Out.resize((size_t)FileSize - FOZ_REF_MAGIC_SIZE);
    return FD->PRead(Out.data(), Out.size(), FOZ_REF_MAGIC_SIZE) == (ssize_t)Out.size();
  }

  bool FOZFile::ReadBlob(uint64_t Offset, std::span<uint8_t> OutBlob) {
    if (FD->PRead(OutBlob.data(), OutBlob.size(), Offset) != (ssize_t)OutBlob.size()) {
      return false;
    }
    return true;
  }

  bool FOZFile::WriteBlob(const MesaFOZ::foz_payload_key& Key, std::span<const std::span<const uint8_t>> BlobChunks, uint64_t& OutBlobOffset) {
    if (ReadOnly || WritePoisoned || !FD) {
      return false;
    }

    // Fossilize stores payload sizes as uint32_t. Validate the complete payload before
    // appending anything, otherwise a truncated header can make the trailing record
    // unreadable after a large or overflowed write.
    uint64_t TotalBlobSize = 0;
    for (const std::span<const uint8_t>& Chunk : BlobChunks) {
      if (Chunk.size() > std::numeric_limits<uint32_t>::max() - TotalBlobSize) {
        return false;
      }
      TotalBlobSize += Chunk.size();
    }

    ssize_t FileSize = FD->Size();
    if (FileSize < 0 || static_cast<uint64_t>(FileSize) >
                          std::numeric_limits<uint64_t>::max() - sizeof(Key.bytes) - sizeof(MesaFOZ::foz_payload_header) - TotalBlobSize) {
      return false;
    }
    uint64_t WriteOffset = static_cast<uint64_t>(FileSize);

    // Every record is append-only. If any component write fails, roll back the
    // incomplete record so a torn append cannot make later index records unreadable.
    auto RollbackAppend = [&]() {
      if (!FD->Truncate(static_cast<uint64_t>(FileSize))) {
        LogMan::Msg::EFmt("DiskCache: failed to roll back incomplete append to {}", FileName);
      }
      return false;
    };

    if (FD->PWrite(Key.bytes, sizeof(Key.bytes), WriteOffset) != sizeof(Key.bytes)) {
      return RollbackAppend();
    }
    WriteOffset += sizeof(Key.bytes);

    MesaFOZ::foz_payload_header ScratchHeader {.payload_size = static_cast<uint32_t>(TotalBlobSize),
                                               .format = MesaFOZ::FOSSILIZE_COMPRESSION_NONE,
                                               .crc = 0, // todo? maybe
                                               .uncompressed_size = static_cast<uint32_t>(TotalBlobSize)};

    if (FD->PWrite(&ScratchHeader, sizeof(ScratchHeader), WriteOffset) != sizeof(ScratchHeader)) {
      return RollbackAppend();
    }
    WriteOffset += sizeof(ScratchHeader);

    OutBlobOffset = WriteOffset;

    for (const std::span<const uint8_t>& Chunk : BlobChunks) {
      if (Chunk.empty()) {
        continue;
      }
      if (FD->PWrite(Chunk.data(), Chunk.size(), WriteOffset) != static_cast<ssize_t>(Chunk.size())) {
        return RollbackAppend();
      }
      WriteOffset += Chunk.size();
    }

    return true;
  }

  static bool ValidateGuestExtents(std::span<const uint32_t> Extents, uint32_t GuestSize) {
    if (Extents.empty() || (Extents.size() & 1) != 0 || GuestSize == 0) {
      return false;
    }
    uint64_t PreviousEnd = 0;
    for (size_t i = 0; i < Extents.size(); i += 2) {
      const uint64_t Offset = Extents[i];
      const uint64_t Length = Extents[i + 1];
      if (Length == 0 || Offset < PreviousEnd || Offset > GuestSize || Length > GuestSize - Offset) {
        return false;
      }
      PreviousEnd = Offset + Length;
    }
    return true;
  }

  static uint64_t GetBlobSizeNeeded(const BlobFixedHeader& Header) {
    return sizeof(BlobFixedHeader) + static_cast<uint64_t>(Header.GuestExtentsCount) * sizeof(uint32_t) +
           static_cast<uint64_t>(Header.HostSize) +
           static_cast<uint64_t>(Header.EntryPointCount) * sizeof(BlobEntryPoint) +
           static_cast<uint64_t>(Header.SmallRelocCount) * sizeof(BlobSmallRelocation) +
           static_cast<uint64_t>(Header.ThunkRelocCount) * sizeof(BlobThunkRelocation) +
           static_cast<uint64_t>(Header.TouchedGuestPagesCount) * sizeof(int64_t) +
           static_cast<uint64_t>(Header.GuestSize);
  }

  bool IndexedDB::Open(const fextl::string& CacheDBName, bool ReadOnly) {
    if (!CacheFOZ.Open(CacheDBName + ".foz", ReadOnly)) {
      return false;
    }
    if (!IndexFOZ.Open(CacheDBName + "_idx.foz", ReadOnly)) {
      return false;
    }

    File::File::FileHandleType CacheFileHandle = CacheFOZ.GetHandle();
    if (FileMapper && CacheFileHandle != (File::File::FileHandleType)-1) {
      CacheFileMapping = reinterpret_cast<uint8_t*>(FileMapper(CacheFileHandle, ReadOnly ? CacheFOZ.Size() : BIG_MAPPING_SIZE));
      CacheFileSize = CacheFOZ.Size();
    }

    this->ReadOnly = ReadOnly;
    MaxSizeBytes = MaxFileSize;
    return true;
  }

  void IndexedDB::PopulateIndex(Index& CacheIndex, bool& FoundMetadata, std::span<const uint8_t> ExpectedMetadata, bool ValidateMetadata) {
    fextl::vector<uint8_t> Data;
    if (!IndexFOZ.ReadAll(Data)) {
      return;
    }

    ssize_t CacheFOZSize = CacheFOZ.Size();
    if (CacheFOZSize < 0) {
      return;
    }

    const uint8_t* IndexDataStart = Data.data();
    const size_t IndexDataSize = Data.size();
    size_t ReadOffset = 0;
    constexpr size_t RecordHeaderSize = sizeof(MesaFOZ::foz_payload_key) + sizeof(MesaFOZ::foz_payload_header);
    while (ReadOffset <= IndexDataSize && IndexDataSize - ReadOffset >= RecordHeaderSize) {
      const auto* FOZKey = reinterpret_cast<const MesaFOZ::foz_payload_key*>(IndexDataStart + ReadOffset);
      ReadOffset += sizeof(MesaFOZ::foz_payload_key);
      const auto* FOZHeader = reinterpret_cast<const MesaFOZ::foz_payload_header*>(IndexDataStart + ReadOffset);
      ReadOffset += sizeof(MesaFOZ::foz_payload_header);

      // Use subtraction-based bounds checks so corrupted lengths cannot wrap size_t
      // on 32-bit hosts and make the index parser read beyond the file buffer.
      if (FOZHeader->payload_size != sizeof(MesaFOZ::mesa_index_db_file_entry) ||
          ReadOffset > IndexDataSize || FOZHeader->payload_size > IndexDataSize - ReadOffset) {
        break;
      }
      const auto* IndexBlobPayload = reinterpret_cast<const MesaFOZ::mesa_index_db_file_entry*>(IndexDataStart + ReadOffset);
      ReadOffset += FOZHeader->payload_size;

      // skip corrupt (carefully) so we don't have to figure that out in the hot path later
      if (IndexBlobPayload->cache_db_file_offset > (uint64_t)CacheFOZSize ||
          IndexBlobPayload->size > (uint64_t)CacheFOZSize - IndexBlobPayload->cache_db_file_offset) {
        continue;
      }
      const bool IsMetadataKey = std::all_of(FOZKey->bytes, FOZKey->bytes + sizeof(FOZKey->bytes), [](uint8_t Byte) { return Byte == 0xFF; });
      if (!IsMetadataKey) {
        uint64_t Key {};
        const char* KeyBegin = reinterpret_cast<const char*>(FOZKey->bytes);
        const char* KeyEnd = reinterpret_cast<const char*>(&FOZKey->bytes[16]);
        const auto ParseResult = std::from_chars(KeyBegin, KeyEnd, Key, 16);
        // Cache files persist across runs and can be truncated or corrupted. Never use
        // an uninitialized hash when a malformed key fails integer parsing.
        if (ParseResult.ec != std::errc {} || ParseResult.ptr != KeyEnd || IndexBlobPayload->hash != Key ||
            IndexBlobPayload->size < sizeof(BlobFixedHeader)) {
          continue;
        }

        BlobFixedHeader BlobHeader {};
        std::span<uint8_t> HeaderBytes {reinterpret_cast<uint8_t*>(&BlobHeader), sizeof(BlobHeader)};
        if (!ReadCacheBlob(IndexBlobPayload->cache_db_file_offset, HeaderBytes) || BlobHeader.GuestSize == 0 ||
            BlobHeader.GuestExtentsCount == 0 || (BlobHeader.GuestExtentsCount & 1) != 0 ||
            GetBlobSizeNeeded(BlobHeader) > IndexBlobPayload->size ||
            static_cast<uint64_t>(BlobHeader.GuestExtentsCount / 2) > BlobHeader.GuestSize) {
          continue;
        }
        fextl::vector<uint32_t> GuestExtents(BlobHeader.GuestExtentsCount);
        std::span<uint8_t> ExtentBytes {reinterpret_cast<uint8_t*>(GuestExtents.data()),
                                        GuestExtents.size() * sizeof(uint32_t)};
        if (!ReadCacheBlob(IndexBlobPayload->cache_db_file_offset + sizeof(BlobHeader), ExtentBytes) ||
            !ValidateGuestExtents(GuestExtents, BlobHeader.GuestSize)) {
          continue;
        }

        // A lookup hash can legitimately have multiple guest-code variants (for example,
        // writable/executable pages after self-modifying-code changes). Keep each distinct
        // guest hash rather than permanently pinning the first variant seen in this DB.
        auto& Entries = CacheIndex[IndexBlobPayload->hash];
        const auto Duplicate = std::find_if(Entries.begin(), Entries.end(), [&](const IndexEntry& Entry) {
          return Entry.GuestSize == BlobHeader.GuestSize && Entry.GuestExtents == GuestExtents &&
                 std::memcmp(&Entry.GuestHash, &BlobHeader.GuestHash, sizeof(BlobHeader.GuestHash)) == 0;
        });
        if (Duplicate == Entries.end() && Entries.size() < MAX_LOOKUP_KEY_VARIANTS) {
          Entries.push_back({this, IndexBlobPayload->cache_db_file_offset, IndexBlobPayload->size,
                             BlobHeader.GuestSize, BlobHeader.GuestHash, std::move(GuestExtents)});
        }
      } else {
        // Treat metadata as present only if the index record and payload match the
        // bucket we opened. A corrupt/stale sentinel must not suppress metadata repair.
        if (IndexBlobPayload->hash != ~uint64_t {}) {
          continue;
        }
        if (ValidateMetadata) {
          if (IndexBlobPayload->size != ExpectedMetadata.size()) {
            continue;
          }
          fextl::vector<uint8_t> MetadataBlob(IndexBlobPayload->size);
          if (!ReadCacheBlob(IndexBlobPayload->cache_db_file_offset, MetadataBlob) ||
              !std::equal(MetadataBlob.begin(), MetadataBlob.end(), ExpectedMetadata.begin(), ExpectedMetadata.end())) {
            continue;
          }
        }
        FoundMetadata = true;
      }
    }
    // could truncate/delete index if we don't end up perfectly at end here
  }

  bool IndexedDB::ReadCacheBlob(uint64_t Offset, std::span<uint8_t> OutBlob) {
    if (CacheFileMapping) {
      const uint64_t CurrentFileSize = CacheFileSize.load(std::memory_order_relaxed);
      // Check ranges with subtraction to avoid unsigned overflow from corrupt index entries.
      if (Offset > CurrentFileSize || OutBlob.size() > CurrentFileSize - Offset) {
        return false;
      }

      // Read-only mappings cover the full file; writable mappings reserve a fixed virtual range.
      // If a requested range falls outside that reservation, use positioned file I/O instead.
      if (ReadOnly || (Offset <= BIG_MAPPING_SIZE && OutBlob.size() <= BIG_MAPPING_SIZE - Offset)) {
        // todo could reduce copies by having a private mapping for relocs, etc
        memcpy(OutBlob.data(), CacheFileMapping + Offset, OutBlob.size());
        return true;
      }
    }
    return CacheFOZ.ReadBlob(Offset, OutBlob);
  }

  bool IndexedDB::StoreCacheBlob(const MesaFOZ::foz_payload_key& Key, std::span<const uint8_t> Blob, Index& Index, std::mutex& IndexMutex) {
    if (ReadOnly) {
      // shouldn't happen
      return false;
    }
    // The FOZ record header and in-memory index both store blob lengths as uint32_t.
    if (Blob.size() > std::numeric_limits<uint32_t>::max()) {
      return false;
    }
    const bool IsMetadataKey = std::all_of(Key.bytes, Key.bytes + sizeof(Key.bytes), [](uint8_t Byte) { return Byte == 0xFF; });
    uint64_t Hash {};
    BlobFixedHeader BlobHeader {};
    fextl::vector<uint32_t> GuestExtents;
    if (!IsMetadataKey) {
      const char* KeyBegin = reinterpret_cast<const char*>(Key.bytes);
      const char* KeyEnd = reinterpret_cast<const char*>(&Key.bytes[16]);
      const auto ParseResult = std::from_chars(KeyBegin, KeyEnd, Hash, 16);
      if (ParseResult.ec != std::errc {} || ParseResult.ptr != KeyEnd || Blob.size() < sizeof(BlobHeader)) {
        return false;
      }
      memcpy(&BlobHeader, Blob.data(), sizeof(BlobHeader));
      if (BlobHeader.GuestSize == 0 || BlobHeader.GuestExtentsCount == 0 ||
          (BlobHeader.GuestExtentsCount & 1) != 0 || GetBlobSizeNeeded(BlobHeader) > Blob.size() ||
          static_cast<uint64_t>(BlobHeader.GuestExtentsCount / 2) > BlobHeader.GuestSize) {
        return false;
      }
      GuestExtents.resize(BlobHeader.GuestExtentsCount);
      memcpy(GuestExtents.data(), Blob.data() + sizeof(BlobHeader), GuestExtents.size() * sizeof(uint32_t));
      if (!ValidateGuestExtents(GuestExtents, BlobHeader.GuestSize)) {
        return false;
      }
    } else {
      Hash = ~uint64_t {};
    }

    auto AlreadyHasVariant = [&]() {
      auto It = Index.find(Hash);
      if (It == Index.end()) {
        return false;
      }
      if (IsMetadataKey) {
        return !It->second.empty();
      }
      return std::any_of(It->second.begin(), It->second.end(), [&](const IndexEntry& Entry) {
        return Entry.GuestSize == BlobHeader.GuestSize && Entry.GuestExtents == GuestExtents &&
               std::memcmp(&Entry.GuestHash, &BlobHeader.GuestHash, sizeof(BlobHeader.GuestHash)) == 0;
      });
    };
    auto IsVariantBucketFull = [&]() {
      auto It = Index.find(Hash);
      return !IsMetadataKey && It != Index.end() && It->second.size() >= MAX_LOOKUP_KEY_VARIANTS;
    };
    {
      std::lock_guard Guard(IndexMutex);
      if (AlreadyHasVariant() || IsVariantBucketFull()) {
        return true;
      }
    }

    if (!CacheFOZ.Lock(STORE_LOCK_TIMEOUT_MS) || !IndexFOZ.Lock(STORE_LOCK_TIMEOUT_MS)) {
      CacheFOZ.Unlock();
      IndexFOZ.Unlock();
      return false;
    }

    // Another writer may have published this hash after our optimistic lookup above.
    // Recheck after acquiring the per-database file locks so concurrent stores don't
    // append duplicate records and consume the bounded cache capacity unnecessarily.
    {
      std::lock_guard Guard(IndexMutex);
      if (AlreadyHasVariant() || IsVariantBucketFull()) {
        CacheFOZ.Unlock();
        IndexFOZ.Unlock();
        return true;
      }
    }

    // Account for both append-only FOZ files before writing either one.
    // When full, keep existing entries readable and decline this new entry.
    const ssize_t CacheBytes = CacheFOZ.Size();
    const ssize_t IndexBytes = IndexFOZ.Size();
    constexpr uint64_t RecordOverhead = sizeof(MesaFOZ::foz_payload_key) + sizeof(MesaFOZ::foz_payload_header);
    constexpr uint64_t IndexEntryBytes = sizeof(MesaFOZ::mesa_index_db_file_entry);
    constexpr uint64_t U64Max = std::numeric_limits<uint64_t>::max();
    if (CacheBytes < 0 || IndexBytes < 0 || Blob.size() > U64Max - RecordOverhead) {
      CacheFOZ.Unlock();
      IndexFOZ.Unlock();
      return false;
    }
    const uint64_t PendingCacheBytes = RecordOverhead + static_cast<uint64_t>(Blob.size());
    const uint64_t PendingIndexBytes = RecordOverhead + IndexEntryBytes;
    // DISKCACHEMAXFILESIZE limits the primary cache data file. The smaller index is
    // independently guarded against integer overflow, but is not charged to the data-file budget.
    if (!CanStoreWithinCapacity(MaxSizeBytes, static_cast<uint64_t>(CacheBytes), 0, PendingCacheBytes, 0) ||
        !CanStoreWithinCapacity(U64Max, static_cast<uint64_t>(IndexBytes), 0, PendingIndexBytes, 0)) {
      CacheFOZ.Unlock();
      IndexFOZ.Unlock();
      // Capacity exhaustion is a cache miss policy decision, not an execution failure.
      return true;
    }

    // write cache side first so we get offset for index
    std::span<const uint8_t> BlobChunks[] = {Blob};
    uint64_t BlobOffset = 0;
    if (!CacheFOZ.WriteBlob(Key, BlobChunks, BlobOffset)) {
      CacheFOZ.Unlock();
      IndexFOZ.Unlock();
      return false;
    }

    MesaFOZ::mesa_index_db_file_entry IndexEntry {.hash = Hash,
                                                  .size = (uint32_t)Blob.size(),
                                                  .last_access_time = 0, // todo..
                                                  .cache_db_file_offset = BlobOffset};

    std::span<const uint8_t> IndexBlobChunks[] = {{(const uint8_t*)&IndexEntry, sizeof(IndexEntry)}};
    uint64_t UnusedIndexBlobOffset = 0;
    if (!IndexFOZ.WriteBlob(Key, IndexBlobChunks, UnusedIndexBlobOffset)) {
      // The cache payload is not reachable until its index record is committed.
      // Remove that orphan append if the index write fails, keeping both files in
      // sync and preserving capacity for subsequent stores.
      if (!CacheFOZ.Truncate(static_cast<uint64_t>(CacheBytes))) {
        LogMan::Msg::EFmt("DiskCache: failed to roll back orphaned cache payload");
      }
      CacheFOZ.Unlock();
      IndexFOZ.Unlock();
      return false;
    }

    // Publish the new mapping size and index entry before dropping the per-file locks:
    // another writer in this process must not append the same guest variant in this window.
    if (CacheFileMapping) {
      CacheFileSize = BlobOffset + Blob.size();
    }

    {
      std::lock_guard Guard(IndexMutex);
      auto& Entries = Index[Hash];
      if (IsMetadataKey) {
        Entries.push_back({this, BlobOffset, static_cast<uint32_t>(Blob.size()), 0, {}});
      } else {
        Entries.push_back({this, BlobOffset, static_cast<uint32_t>(Blob.size()), BlobHeader.GuestSize, BlobHeader.GuestHash,
                           std::move(GuestExtents)});
      }
    }

    CacheFOZ.Unlock();
    IndexFOZ.Unlock();
    return true;
  }

  bool DiskCache::OpenCacheDB(const fextl::string& CacheDBName, bool ReadOnly, std::span<const uint8_t> ExpectedMetadata) {
    fextl::unique_ptr<IndexedDB> CurDB;

    if (!ReadOnly && RWCacheDB) {
      // rw already opened, just support one
      return false;
    }

    CurDB = fextl::make_unique<IndexedDB>();
    if (!CurDB) {
      return false;
    }

    if (!CurDB->Open(CacheDBName, ReadOnly)) {
      CurDB.reset();
      return false;
    }

    CurDB->PopulateIndex(Index, FoundMetadata, ExpectedMetadata, !ReadOnly);

    if (ReadOnly) {
      ROCacheDBs.push_back(std::move(CurDB));
    } else {
      RWCacheDB = std::move(CurDB);
    }

    return true;
  }

  FEX_DEFAULT_VISIBILITY void SetFileMapper(FileMapperFunc Func) {
    FileMapper = Func;
  }

  static bool IsMachineBucketDirectoryName(std::string_view Name) {
    if (Name.size() != 16) {
      return false;
    }
    for (const char C : Name) {
      const bool IsDigit = C >= '0' && C <= '9';
      const bool IsLowerHex = C >= 'a' && C <= 'f';
      const bool IsUpperHex = C >= 'A' && C <= 'F';
      if (!IsDigit && !IsLowerHex && !IsUpperHex) {
        return false;
      }
    }
    return true;
  }

  // Only prune generated machine-bucket directories. Never recurse through symlinks or
  // remove arbitrary user-owned directories that happen to live beside the cache.
  static void PruneStaleCacheBuckets(std::string_view CacheBase, std::string_view ActiveMachineBucket) {
    FEX_CONFIG_OPT(DiskCachePruneStaleEntries, DISKCACHEPRUNESTALEENTRIES);
    if (!DiskCachePruneStaleEntries) {
      return;
    }

    fextl::string RootPath(CacheBase);
    std::error_code IterationError;
    const std::filesystem::directory_iterator End;
    std::filesystem::directory_iterator It(std::filesystem::path(RootPath.c_str()),
                                           std::filesystem::directory_options::skip_permission_denied, IterationError);
    if (IterationError) {
      return;
    }

    for (; It != End; It.increment(IterationError)) {
      if (IterationError) {
        IterationError.clear();
        continue;
      }

      const std::string Name = It->path().filename().string();
      if (!IsMachineBucketDirectoryName(Name) || Name == ActiveMachineBucket) {
        continue;
      }

      std::error_code StatusError;
      if (It->symlink_status(StatusError).type() != std::filesystem::file_type::directory || StatusError) {
        continue;
      }

      std::error_code RemoveError;
      std::filesystem::remove_all(It->path(), RemoveError);
    }
  }

  void DiskCache::Init(FEXCore::Context::ContextImpl* CTX) {
    this->CTX = CTX;
    MaxWriteBufferBytes = std::min<uint64_t>(static_cast<uint64_t>(MemoryBufferMB()) * 1024ULL * 1024ULL, 1ULL << 30);

    if (!EnableDiskCache) {
      return;
    }

    fextl::string SerializedConfig = FEXCore::Config::SerializeForCache();

    // Keep machine compatibility separate from process/code-generation identity, as in
    // upstream 2610. This isolates cache generations by FEX cache format and host
    // features, while 32/64-bit mode and serialized configuration select a DB inside
    // that machine bucket. Existing single-hash cache directories remain untouched.
    const uint64_t HostFeaturesHash = CTX->HostFeatures.HashForCaching();

    struct __attribute__((packed)) {
      uint16_t FormatVersion;
      uint64_t HostFeaturesHash;
    } MachineBucketData = {FormatVersion, HostFeaturesHash};

    fextl::vector<uint8_t> BucketBytes(sizeof(MachineBucketData) + 2 + SerializedConfig.size());
    memcpy(BucketBytes.data(), &MachineBucketData, sizeof(MachineBucketData));
    BucketBytes[sizeof(MachineBucketData)] = CTX->Config.Is64BitMode;
    // This 2605 baseline has a single AArch64 host backend and no HostType
    // discriminator yet; reserve the upstream process-bucket byte as zero.
    BucketBytes[sizeof(MachineBucketData) + 1] = 0;
    memcpy(BucketBytes.data() + sizeof(MachineBucketData) + 2, SerializedConfig.data(), SerializedConfig.size());

    const uint64_t MachineBucketHash = XXH3_64bits(BucketBytes.data(), sizeof(MachineBucketData));
    const uint64_t ProcessBucketHash = XXH3_64bits(BucketBytes.data() + sizeof(MachineBucketData), 2 + SerializedConfig.size());
    BucketHash.high64 = MachineBucketHash;
    BucketHash.low64 = ProcessBucketHash;

    fextl::string BasePath = BasePathOverride();
    if (BasePath.empty()) {
      BasePath = FEXCore::Config::GetCacheDirectory() + "DiskCache/";
    }
    // A user-specified cache path need not include a trailing path separator.
    if (!BasePath.empty() && BasePath.back() != '/') {
      BasePath += "/";
    }
    const fextl::string MachineBucketName = fextl::fmt::format("{:016x}", MachineBucketHash);
    FHU::Filesystem::CreateDirectories(BasePath);
    PruneStaleCacheBuckets(BasePath, MachineBucketName);
    BasePath += MachineBucketName + "/";
    FHU::Filesystem::CreateDirectories(BasePath);

    if (!MapDiskCacheFiles) {
      FileMapper = nullptr;
    }

    fextl::string RWDBBasePath = BasePath + fextl::fmt::format("RWCacheDB_{:016x}", ProcessBucketHash);
    OpenCacheDB(RWDBBasePath, false, BucketBytes);

    if (RWCacheDB && !FoundMetadata) {
      // we just opened a fresh cache, add a metadata blob
      MesaFOZ::foz_payload_key MetadataKey;
      memset(MetadataKey.bytes, 0xFF, sizeof(MetadataKey));
      RWCacheDB->StoreCacheBlob(MetadataKey, {BucketBytes.data(), BucketBytes.size()}, Index, IndexLock);
      // Metadata is stored in the same index map as code blobs; remove only its
      // sentinel key so any valid code entries recovered from the DB remain usable.
      Index.erase(~uint64_t {});
    }

    std::string_view RONames = RODBNames();
    while (!RONames.empty()) {
      const auto Delim = RONames.find(',');
      const std::string_view ROName = RONames.substr(0, Delim);
      if (!ROName.empty()) {
        fextl::string RODBBasePath = BasePath;
        RODBBasePath += ROName;
        OpenCacheDB(RODBBasePath, true, BucketBytes);
      }
      if (Delim == std::string_view::npos) {
        break;
      }
      // advance to next
      RONames.remove_prefix(Delim + 1);
    }

    // The zero-buffer mode is deliberately synchronous and must also work for
    // embedders/tests that do not install a frontend thread-creation callback.
    // Only create the background writer when queued writes are actually enabled.
    if (IsWritingDiskCache() && MaxWriteBufferBytes != 0) {
      Writer = fextl::make_unique<WorkQueueThread>();
    }
  }

  // Include the backing file identity in the key. A module-relative offset alone
  // can collide when different executable files are mapped at the same address.
  static uint64_t MakeFileBackedCodeKey(uint64_t GuestRIP, const ExecutableFileSectionInfo& Region) {
    struct __attribute__((packed)) {
      uint64_t GuestOffset;
      uint64_t FileId;
    } FileBackedKey = {GuestRIP - Region.FileStartVA, Region.FileInfo.FileId};

    return XXH3_64bits(&FileBackedKey, sizeof(FileBackedKey));
  }

  std::optional<uint64_t> DiskCache::MakeCodeKey(Core::InternalThreadState* Thread,
                                                       const std::optional<ExecutableFileSectionInfo>& Region,
                                                       uint64_t GuestRIP, bool DisownDecoderBuffer) {
    if (Region && Region->FileStartVA != 0) {
      return MakeFileBackedCodeKey(GuestRIP, *Region);
    }
    if (!AnonCaching) {
      return std::nullopt;
    }

    const auto RangeInfo = CTX->SyscallHandler->QueryGuestExecutableRange(Thread, GuestRIP);
    if (RangeInfo.Size == 0 || RangeInfo.Base > GuestRIP) {
      return std::nullopt;
    }
    const uint64_t Offset = GuestRIP - RangeInfo.Base;
    if (Offset >= RangeInfo.Size) {
      return std::nullopt;
    }

    // Core::CompileBlock prepares the requested block before cache lookup so the
    // cache key can include the actual MaxInst / multi-block footprint. Re-decoding
    // here with a hard-coded limit of four instructions would overwrite that decoder
    // state before a cache hit returns to the dispatcher. Reuse an existing decode
    // only when it starts at this exact entrypoint; otherwise build the small,
    // deterministic anonymous-code identity decode as before.
    const auto* ExistingDecodedInfo = Thread->FrontendDecoder->GetDecodedBlockInfo();
    const bool HasRequestedEntryDecode =
      ExistingDecodedInfo && !ExistingDecodedInfo->Blocks.empty() &&
      ExistingDecodedInfo->Blocks.front().Entry == GuestRIP &&
      ExistingDecodedInfo->Blocks.front().BlockStatus == Frontend::Decoder::DecodedBlockStatus::SUCCESS;
    if (!HasRequestedEntryDecode) {
      Thread->FrontendDecoder->DecodeInstructionsAtEntry(Thread, reinterpret_cast<const uint8_t*>(GuestRIP), GuestRIP, 4);
    }
    const auto* DecodedInfo = Thread->FrontendDecoder->GetDecodedBlockInfo();
    if (!DecodedInfo || DecodedInfo->Blocks.empty()) {
      if (DisownDecoderBuffer) {
        Thread->FrontendDecoder->DelayedDisownBuffer();
      }
      return std::nullopt;
    }
    XXH3_state_t HashState;
    XXH3_64bits_reset(&HashState);
    bool Valid = true;
    for (const auto& SubBlock : DecodedInfo->Blocks) {
      if (SubBlock.BlockStatus != Frontend::Decoder::DecodedBlockStatus::SUCCESS ||
          SubBlock.Size > std::numeric_limits<uint64_t>::max() - SubBlock.Entry) {
        Valid = false;
        break;
      }
      // Unsigned relative offsets are intentional: backward branch targets remain
      // deterministic relative to the candidate entrypoint.
      const uint64_t BlockOffset = SubBlock.Entry - GuestRIP;
      const uint64_t BlockSize = SubBlock.Size;
      XXH3_64bits_update(&HashState, &BlockOffset, sizeof(BlockOffset));
      XXH3_64bits_update(&HashState, &BlockSize, sizeof(BlockSize));
      uint64_t HashStart = SubBlock.Entry;
      const uint64_t BlockEnd = SubBlock.Entry + SubBlock.Size;
      for (const auto& Mask : SubBlock.DataMasks) {
        if (Mask.FieldAddress < HashStart || Mask.FieldAddress > BlockEnd ||
            Mask.ValueSize > BlockEnd - Mask.FieldAddress) {
          Valid = false;
          break;
        }
        if (Mask.FieldAddress > HashStart) {
          XXH3_64bits_update(&HashState, reinterpret_cast<const uint8_t*>(HashStart), Mask.FieldAddress - HashStart);
        }
        HashStart = Mask.FieldAddress + Mask.ValueSize;
      }
      if (!Valid) {
        break;
      }
      if (HashStart < BlockEnd) {
        XXH3_64bits_update(&HashState, reinterpret_cast<const uint8_t*>(HashStart), BlockEnd - HashStart);
      }
    }
    // Lookup callers own decoder-buffer cleanup: a cache hit disowns it in
    // CompileBlock, while a miss reuses it during IR generation. Store runs
    // after compilation and must release its temporary decode buffer here.
    if (DisownDecoderBuffer) {
      Thread->FrontendDecoder->DelayedDisownBuffer();
    }
    if (!Valid) {
      return std::nullopt;
    }
    return XXH3_64bits_digest(&HashState);
  }

  // Match cache entries to the decoded guest-block footprint, not only the
  // executable's file offset. The same RIP can compile to different host blocks
  // under different MaxInst / multi-block settings.
  static std::optional<uint64_t> MakeBlockFootprint(const Frontend::Decoder::DecodedBlockInformation* DecodedInfo,
                                                     uint64_t GuestRIP) {
    if (!DecodedInfo || DecodedInfo->Blocks.empty()) {
      return std::nullopt;
    }

    XXH3_state_t HashState;
    XXH3_64bits_reset(&HashState);
    for (const auto& SubBlock : DecodedInfo->Blocks) {
      if (SubBlock.BlockStatus != Frontend::Decoder::DecodedBlockStatus::SUCCESS ||
          SubBlock.Size > std::numeric_limits<uint64_t>::max() - SubBlock.Entry) {
        return std::nullopt;
      }

      const uint64_t BlockOffset = SubBlock.Entry - GuestRIP;
      const uint64_t BlockSize = SubBlock.Size;
      XXH3_64bits_update(&HashState, &BlockOffset, sizeof(BlockOffset));
      XXH3_64bits_update(&HashState, &BlockSize, sizeof(BlockSize));
      for (const auto& Mask : SubBlock.DataMasks) {
        if (Mask.FieldAddress < SubBlock.Entry || Mask.FieldAddress > SubBlock.Entry + SubBlock.Size ||
            Mask.ValueSize > SubBlock.Entry + SubBlock.Size - Mask.FieldAddress) {
          return std::nullopt;
        }
        const uint64_t MaskOffset = Mask.FieldAddress - GuestRIP;
        XXH3_64bits_update(&HashState, &MaskOffset, sizeof(MaskOffset));
        XXH3_64bits_update(&HashState, &Mask.ValueSize, sizeof(Mask.ValueSize));
      }
    }
    return XXH3_64bits_digest(&HashState);
  }

  uint64_t DiskCache::MakeBlobKey(Core::InternalThreadState* Thread, const uint64_t CodeKey, uint64_t BlockFootprint,
                                  bool Writable, bool MonoBackpatcher) {
    struct __attribute__((packed)) {
      uint64_t CodeKey;
      XXH128_hash_t BucketHash;
      uint64_t BlockFootprint;
      uint8_t Flags;
    } BlobKeyBytes = {CodeKey, BucketHash, BlockFootprint, 0};

    if (Writable) {
      BlobKeyBytes.Flags |= 1 << 0;
    }
    if (CTX->AreMonoHacksActive()) {
      BlobKeyBytes.Flags |= 1 << 1;
    }
    if (Thread->CurrentFrame->State.flags[X86State::RFLAG_TF_RAW_LOC]) {
      BlobKeyBytes.Flags |= 1 << 2;
    }
    if (MonoBackpatcher) {
      BlobKeyBytes.Flags |= 1 << 3;
    }

    return XXH3_64bits(&BlobKeyBytes, sizeof(BlobKeyBytes));
  }

  static uint64_t MakeMemoryCacheKey(uint64_t LookupKey, uint32_t GuestSize, const XXH128_hash_t& GuestHash) {
    struct __attribute__((packed)) {
      uint64_t LookupKey;
      uint32_t GuestSize;
      XXH128_hash_t GuestHash;
    } KeyData {LookupKey, GuestSize, GuestHash};
    return XXH3_64bits(&KeyData, sizeof(KeyData));
  }

  fextl::shared_ptr<fextl::vector<uint8_t>> DiskCache::LookupMemoryCache(uint64_t Key) {
    std::lock_guard Guard(MemoryCacheLock);
    auto It = MemoryCacheEntries.find(Key);
    if (It == MemoryCacheEntries.end()) {
      return {};
    }

    MemoryCacheLRU.splice(MemoryCacheLRU.begin(), MemoryCacheLRU, It->second.LRUPosition);
    return It->second.Blob;
  }

  fextl::shared_ptr<fextl::vector<uint8_t>> DiskCache::StoreMemoryCache(uint64_t Key, fextl::vector<uint8_t>&& Blob) {
    const uint64_t MaxBytes = MemoryCacheSize;
    if (MaxBytes == 0 || Blob.empty() || Blob.size() > MaxBytes) {
      // Do not consume the caller's buffer when it does not fit in the LRU.
      return {};
    }

    // Adopt the existing buffer instead of copying every byte into a second allocation.
    auto CachedBlob = fextl::make_shared<fextl::vector<uint8_t>>(std::move(Blob));

    std::lock_guard Guard(MemoryCacheLock);
    auto Existing = MemoryCacheEntries.find(Key);
    if (Existing != MemoryCacheEntries.end()) {
      MemoryCacheCurrentBytes -= Existing->second.Blob->size();
      MemoryCacheLRU.erase(Existing->second.LRUPosition);
      MemoryCacheEntries.erase(Existing);
    }

    while (!MemoryCacheLRU.empty() && MemoryCacheCurrentBytes > MaxBytes - CachedBlob->size()) {
      auto Last = std::prev(MemoryCacheLRU.end());
      auto Entry = MemoryCacheEntries.find(*Last);
      if (Entry != MemoryCacheEntries.end()) {
        MemoryCacheCurrentBytes -= Entry->second.Blob->size();
        MemoryCacheEntries.erase(Entry);
      }
      MemoryCacheLRU.erase(Last);
    }

    MemoryCacheLRU.push_front(Key);
    MemoryCacheEntries.emplace(Key, MemoryCacheEntry {CachedBlob, MemoryCacheLRU.begin()});
    MemoryCacheCurrentBytes += CachedBlob->size();
    return CachedBlob;
  }

  std::optional<CodeHitData> DiskCache::Lookup(Core::InternalThreadState* Thread, std::optional<ExecutableFileSectionInfo> Region,
                                               uint64_t GuestRIP, std::optional<uint64_t>& GuestCodeKey) {
    if (!IsReadingDiskCache()) {
      return std::nullopt;
    }
    const auto BlockFootprint = MakeBlockFootprint(Thread->FrontendDecoder->GetDecodedBlockInfo(), GuestRIP);
    if (!BlockFootprint) {
      return std::nullopt;
    }
    const auto CodeKeyValue = MakeCodeKey(Thread, Region, GuestRIP, false);
    if (!CodeKeyValue) {
      return std::nullopt;
    }
    const uint64_t CodeKey = *CodeKeyValue;
    GuestCodeKey = CodeKey;
    auto RangeInfo = CTX->SyscallHandler->QueryGuestExecutableRange(Thread, GuestRIP);
    uint64_t Hash = MakeBlobKey(Thread, CodeKey, *BlockFootprint, RangeInfo.Writable,
                                GuestRIP == CTX->GetMonoBackPatcherBlock());

    fextl::vector<IndexEntry> Candidates;
    {
      std::lock_guard Guard(IndexLock);
      auto It = Index.find(Hash);
      if (It == Index.end()) {
        // definite miss
        return std::nullopt;
      }
      Candidates = It->second;
    }

    // Validate the candidate sizes against the live executable mapping first.
    if (RangeInfo.Size == 0 || RangeInfo.Base > GuestRIP) {
      return std::nullopt;
    }
    const uint64_t GuestOffsetInRange = GuestRIP - RangeInfo.Base;
    if (GuestOffsetInRange >= RangeInfo.Size) {
      return std::nullopt;
    }
    const uint64_t Available = RangeInfo.Size - GuestOffsetInRange;

    // A stable key can have several guest-code variants. Select the variant whose stored
    // guest hash matches the current code, instead of letting an old SMC version shadow
    // every later version indefinitely.
    IndexEntry Entry {};
    XXH128_hash_t LiveGuestHash {};
    bool FoundGuestVariant = false;
    for (const auto& Candidate : Candidates) {
      if (Candidate.GuestSize == 0 || Available < Candidate.GuestSize) {
        continue;
      }
      if (!ValidateGuestExtents(Candidate.GuestExtents, Candidate.GuestSize)) {
        continue;
      }
      XXH3_state_t HashState;
      XXH3_128bits_reset(&HashState);
      for (size_t ExtentIdx = 0; ExtentIdx < Candidate.GuestExtents.size(); ExtentIdx += 2) {
        XXH3_128bits_update(&HashState, reinterpret_cast<const uint8_t*>(GuestRIP) + Candidate.GuestExtents[ExtentIdx],
                            Candidate.GuestExtents[ExtentIdx + 1]);
      }
      const XXH128_hash_t CandidateHash = XXH3_128bits_digest(&HashState);
      if (std::memcmp(&CandidateHash, &Candidate.GuestHash, sizeof(CandidateHash)) == 0) {
        Entry = Candidate;
        LiveGuestHash = CandidateHash;
        FoundGuestVariant = true;
        break;
      }
    }
    if (!FoundGuestVariant) {
      return std::nullopt;
    }

    const uint64_t MemoryCacheKey = MakeMemoryCacheKey(Hash, Entry.GuestSize, Entry.GuestHash);
    CodeHitData HitData;
    auto CachedBlob = LookupMemoryCache(MemoryCacheKey);
    const bool FoundInMemoryCache = CachedBlob && CachedBlob->size() == Entry.Size;
    if (FoundInMemoryCache) {
      // Match upstream: use the LRU as a read source, but give this lookup an
      // independent writable blob so applying relocations never changes cached bytes.
      HitData.Blob.assign(CachedBlob->begin(), CachedBlob->end());
    } else {
      HitData.Blob.resize(Entry.Size);
      if (!Entry.DB->ReadCacheBlob(Entry.Offset, HitData.Blob)) {
        return std::nullopt;
      }
    }
    uint8_t* BlobData = HitData.Blob.data();

    if (Entry.Size < sizeof(BlobFixedHeader)) {
      return std::nullopt;
    }
    BlobFixedHeader Header {};
    memcpy(&Header, BlobData, sizeof(Header));

    if (Header.GuestSize != Entry.GuestSize ||
        std::memcmp(&Header.GuestHash, &Entry.GuestHash, sizeof(Header.GuestHash)) != 0 ||
        std::memcmp(&LiveGuestHash, &Header.GuestHash, sizeof(Header.GuestHash)) != 0) {
      return std::nullopt;
    }

    // Validate the serialized lengths using 64-bit arithmetic. The cache is persistent
    // input, so a corrupt header must not wrap SizeNeeded and produce out-of-bounds spans.
    const uint64_t SizeNeeded = GetBlobSizeNeeded(Header);
    if (SizeNeeded > Entry.Size || Header.GuestExtentsCount != Entry.GuestExtents.size()) {
      return std::nullopt;
    }

    const size_t GuestExtentsBytes = static_cast<size_t>(Header.GuestExtentsCount) * sizeof(uint32_t);
    if (std::memcmp(BlobData + sizeof(Header), Entry.GuestExtents.data(), GuestExtentsBytes) != 0) {
      return std::nullopt;
    }
    const size_t HostCodeOffset = sizeof(Header) + GuestExtentsBytes;
    HitData.HostCode = {BlobData + HostCodeOffset, Header.HostSize};
    HitData.EntryPoints = {reinterpret_cast<const BlobEntryPoint*>(BlobData + HostCodeOffset + Header.HostSize),
                           Header.EntryPointCount};

    auto* SmallRelocs = reinterpret_cast<const BlobSmallRelocation*>(
      BlobData + HostCodeOffset + Header.HostSize + Header.EntryPointCount * sizeof(BlobEntryPoint));
    auto* ThunkRelocs = reinterpret_cast<const BlobThunkRelocation*>(
      reinterpret_cast<const uint8_t*>(SmallRelocs) + Header.SmallRelocCount * sizeof(BlobSmallRelocation));

    HitData.Relocations.reserve(Header.SmallRelocCount + Header.ThunkRelocCount);
    for (uint32_t i = 0; i < Header.SmallRelocCount; ++i) {
      const auto& SmallReloc = SmallRelocs[i];
      FEXCore::CPU::Relocation Reloc = FEXCore::CPU::Relocation::Default();
      Reloc.Header.Type = (CPU::RelocationTypes)SmallReloc.Type;
      Reloc.Header.Offset = SmallReloc.Offset;
      switch (SmallReloc.Type) {
      case uint8_t(CPU::RelocationTypes::RELOC_NAMED_SYMBOL_LITERAL):
        Reloc.NamedSymbolLiteral.Symbol = CPU::RelocNamedSymbolLiteral::NamedSymbol(SmallReloc.Named.Symbol);
        break;
      // Keep guest RIP relocation targets block-relative here. ApplyCodeRelocations
      // adds GuestRIP when materializing the target; rebasing here as well would
      // apply the guest base twice and send cached branches to the wrong address.
      case uint8_t(CPU::RelocationTypes::RELOC_GUEST_RIP_LITERAL):
        Reloc.GuestRIP.GuestRIP = SmallReloc.RIPLiteral.GuestRIP;
        break;
      case uint8_t(CPU::RelocationTypes::RELOC_GUEST_RIP_MOVE):
        Reloc.GuestRIP.RegisterIndex = SmallReloc.RIPMove.RegisterIndex;
        Reloc.GuestRIP.GuestRIP = SmallReloc.RIPMove.GuestRIP;
        break;
      case uint8_t(CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_DATA_MOVE):
      case uint8_t(CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_CRC_MOVE):
      case uint8_t(CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_LITERAL):
      case uint8_t(CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_MOVE): {
        const bool IsCRCRelocation = SmallReloc.Type == uint8_t(CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_CRC_MOVE);
        const bool IsRIPRelocation =
          SmallReloc.Type == uint8_t(CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_LITERAL) ||
          SmallReloc.Type == uint8_t(CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_MOVE);
        const uint64_t SiteOffset = SmallReloc.PatchableData.SiteOffset;
        const uint64_t ValueSize = SmallReloc.PatchableData.ValueSize;
        const uint64_t MaxPatchSize = IsCRCRelocation ? 16 : (IsRIPRelocation ? 4 : sizeof(uint64_t));
        if (ValueSize == 0 || ValueSize > MaxPatchSize ||
            (!IsCRCRelocation && SmallReloc.PatchableData.SignExtend > 1) ||
            (IsCRCRelocation && SmallReloc.PatchableData.SignExtend != 0) ||
            SiteOffset > Header.GuestSize || ValueSize > Header.GuestSize - SiteOffset ||
            SiteOffset > std::numeric_limits<uint64_t>::max() - GuestRIP) {
          return std::nullopt;
        }
        Reloc.GuestPatchableData.RegisterIndex = SmallReloc.PatchableData.RegisterIndex;
        Reloc.GuestPatchableData.ValueSize = static_cast<uint8_t>(ValueSize);
        Reloc.GuestPatchableData.SignExtend = !IsCRCRelocation && SmallReloc.PatchableData.SignExtend != 0;
        Reloc.GuestPatchableData.SiteAddress = GuestRIP + SiteOffset;
        break;
      }
      default: return std::nullopt;
      }
      HitData.Relocations.push_back(Reloc);
    }
    for (uint32_t i = 0; i < Header.ThunkRelocCount; ++i) {
      const auto& BigReloc = ThunkRelocs[i];
      FEXCore::CPU::Relocation Reloc = FEXCore::CPU::Relocation::Default();
      Reloc.NamedThunkMove.Header.Offset = BigReloc.Offset;
      Reloc.NamedThunkMove.Header.Type = CPU::RelocationTypes::RELOC_NAMED_THUNK_MOVE;
      Reloc.NamedThunkMove.RegisterIndex = BigReloc.RegisterIndex;
      memcpy(&Reloc.NamedThunkMove.Symbol, BigReloc.SymbolHash, sizeof(BigReloc.SymbolHash));
      HitData.Relocations.push_back(Reloc);
    }

    auto* PageOffsets =
      reinterpret_cast<const int64_t*>(reinterpret_cast<const uint8_t*>(ThunkRelocs) + Header.ThunkRelocCount * sizeof(BlobThunkRelocation));
    HitData.GuestPages.reserve(Header.TouchedGuestPagesCount);
    for (uint32_t i = 0; i < Header.TouchedGuestPagesCount; ++i) {
      // Guest-page offsets come from persistent cache data. Reconstruct them using
      // guest address width, then reject malformed entries that aren't page aligned.
      uint64_t GuestPage = GuestRIP + static_cast<uint64_t>(PageOffsets[i]);
      if (!CTX->Config.Is64BitMode) {
        GuestPage = static_cast<uint32_t>(GuestPage);
      }
      if ((GuestPage & (FEXCore::Utils::FEX_PAGE_SIZE - 1)) != 0) {
        return std::nullopt;
      }
      HitData.GuestPages.push_back(GuestPage);
    }

    // Preserve the serialized, unrelocated bytes in the memory LRU before
    // applying relocations to this lookup's private writable copy.
    if (!FoundInMemoryCache) {
      fextl::vector<uint8_t> MemoryCacheBlob(HitData.Blob.begin(), HitData.Blob.end());
      (void)StoreMemoryCache(MemoryCacheKey, std::move(MemoryCacheBlob));
    }

    // Match upstream's cache-hit architecture: relocate the private writable
    // blob inside Lookup, before Core can load it into the executable code buffer.
    if (!CTX->CodeCache.ApplyCodeRelocations(GuestRIP, std::as_writable_bytes(HitData.HostCode), HitData.Relocations, false)) {
      return std::nullopt;
    }
    return HitData;
  }

  struct DiskCache::CacheStoreWorkItem final : WorkQueueThread::WorkItem {
    DiskCache* Self;
    IndexedDB* DB;
    MesaFOZ::foz_payload_key Key;
    fextl::shared_ptr<fextl::vector<uint8_t>> Blob;
    uint64_t ReservedBytes {};
    CacheStoreWorkItem(DiskCache* Self, IndexedDB* DB, const MesaFOZ::foz_payload_key& Key, fextl::shared_ptr<fextl::vector<uint8_t>> Blob, uint64_t ReservedBytes)
      : Self(Self)
      , DB(DB)
      , Key(Key)
      , Blob(std::move(Blob))
      , ReservedBytes(ReservedBytes) {}
    ~CacheStoreWorkItem() override {
      if (ReservedBytes) {
        Self->PendingWriteBufferBytes.fetch_sub(ReservedBytes, std::memory_order_relaxed);
      }
    }
    void Run() override {
      DB->StoreCacheBlob(Key, *Blob, Self->Index, Self->IndexLock);
    }
  };

  bool DiskCache::Store(Core::InternalThreadState* Thread, std::optional<ExecutableFileSectionInfo> Region, uint64_t GuestRIP,
                        uint64_t GuestCodeKey, std::span<const uint8_t> GuestCode, const CPU::CPUBackend::CompiledCode& CompiledCode,
                        std::span<const FEXCore::CPU::Relocation> Relocations, const Frontend::Decoder::DecodedBlockInformation* DecodedBlockInfo) {
    if (!IsWritingDiskCache()) {
      return false;
    }
    if (!DecodedBlockInfo) {
      return false;
    }
    if ((!Region || Region->FileStartVA == 0) &&
        reinterpret_cast<uint64_t>(GuestCode.data()) != GuestRIP) {
      // Anonymous entrypoint hashing/rebasing uses GuestRIP as the blob base;
      // refuse blocks whose frontend code span begins at a different address.
      return false;
    }

    // check for any reloc targets outside of our jurisdiction
    // todo what are they exactly? caching those blocks is great when it works, so need to figure this out and make finer-grained if we can
    if (RelocationFilter) {
      for (const auto& Reloc : Relocations) {
        if (Reloc.Header.Type != CPU::RelocationTypes::RELOC_GUEST_RIP_LITERAL && Reloc.Header.Type != CPU::RelocationTypes::RELOC_GUEST_RIP_MOVE) {
          continue;
        }
        uint64_t Target = Reloc.GuestRIP.GuestRIP;
        if (Region && Region->FileStartVA != 0) {
          if (Target >= Region->BeginVA && Target < Region->EndVA) {
            continue;
          }
          // Relocations into another PT_LOAD segment or .bss in the same ELF image
          // are stable under relocation of the image as a whole.
          if (Region->FileInfo.MappedSize && Target >= Region->FileStartVA &&
              Target - Region->FileStartVA < Region->FileInfo.MappedSize) {
            continue;
          }
          auto TargetSection = CTX->SyscallHandler->LookupExecutableFileSection(Thread, Target);
          if (!TargetSection || TargetSection->FileInfo.FileId != Region->FileInfo.FileId) {
            // We cannot encode the target's identity/offset safely.
            return false;
          }
        } else {
          // Without a backing ELF identity only in-block RIP targets have a stable
          // relocation under rebasing. Refuse other targets instead of serializing a
          // process-specific absolute pointer into a reusable cache entry.
          if (Target < GuestRIP || Target - GuestRIP >= GuestCode.size()) {
            return false;
          }
        }
      }
    }

    uint32_t SmallRelocCount = 0;
    uint32_t ThunkRelocCount = 0;
    for (const auto& Reloc : Relocations) {
      // relocs aren't cleared every time if IsGeneratingCache, so filter just in case
      if (Reloc.Header.Type == CPU::RelocationTypes::RELOC_NAMED_THUNK_MOVE) {
        ThunkRelocCount++;
      } else {
        SmallRelocCount++;
      }
    }

    // BlobFixedHeader and the FOZ payload use 32-bit lengths/counts. Reject blocks
    // that cannot be represented instead of truncating fields or overflowing offsets.
    constexpr size_t MaxSerializedField = std::numeric_limits<uint32_t>::max();
    if (CompiledCode.Size > MaxSerializedField || GuestCode.size() > MaxSerializedField ||
        CompiledCode.EntryPoints.size() > MaxSerializedField || Relocations.size() > MaxSerializedField ||
        (DecodedBlockInfo && DecodedBlockInfo->CodePages.size() > MaxSerializedField) ||
        SmallRelocCount > MaxSerializedField || ThunkRelocCount > MaxSerializedField) {
      return false;
    }
    const uint32_t EntryPointCount = static_cast<uint32_t>(CompiledCode.EntryPoints.size());
    const uint32_t TouchedGuestPagesCount = DecodedBlockInfo ? static_cast<uint32_t>(DecodedBlockInfo->CodePages.size()) : 0;
    // The JIT emitter aligns every compiled block to this contract. Reject a
    // malformed block instead of padding its serialized size and disagreeing with
    // the embedded JITBlockTail::Size.
    if ((CompiledCode.Size % 16) != 0) {
      return false;
    }

    // Validate every serialized patch site against the guest-code payload. CRC sites
    // span an instruction (up to 16 bytes); data relocations patch an immediate of at most 8.
    for (const auto& Reloc : Relocations) {
      const bool IsDataReloc = Reloc.Header.Type == CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_DATA_MOVE;
      const bool IsCRCReloc = Reloc.Header.Type == CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_CRC_MOVE;
      const bool IsRIPReloc = Reloc.Header.Type == CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_LITERAL ||
                              Reloc.Header.Type == CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_MOVE;
      if (!IsDataReloc && !IsCRCReloc && !IsRIPReloc) {
        continue;
      }
      const uint64_t SiteAddress = Reloc.GuestPatchableData.SiteAddress;
      const uint64_t SiteSize = Reloc.GuestPatchableData.ValueSize;
      const uint64_t MaxSiteSize = IsCRCReloc ? 16 : (IsRIPReloc ? 4 : sizeof(uint64_t));
      if (SiteAddress < GuestRIP || SiteSize == 0 || SiteSize > MaxSiteSize ||
          SiteAddress - GuestRIP > GuestCode.size() ||
          SiteSize > GuestCode.size() - (SiteAddress - GuestRIP)) {
        return false;
      }
    }

    // Only omit fields that have an emitted patchable-data relocation. Anything
    // that the backend does not know how to patch remains byte-exact in the hash.
    fextl::vector<std::pair<uint32_t, uint32_t>> PatchableFields;
    for (const auto& Reloc : Relocations) {
      const bool IsDataReloc = Reloc.Header.Type == CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_DATA_MOVE;
      const bool IsRIPReloc = Reloc.Header.Type == CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_LITERAL ||
                              Reloc.Header.Type == CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_MOVE;
      if (!IsDataReloc && !IsRIPReloc) {
        continue;
      }
      const uint64_t SiteAddress = Reloc.GuestPatchableData.SiteAddress;
      const uint64_t SiteSize = Reloc.GuestPatchableData.ValueSize;
      if (SiteAddress < GuestRIP || SiteAddress - GuestRIP > GuestCode.size() ||
          SiteSize == 0 || SiteSize > GuestCode.size() - (SiteAddress - GuestRIP)) {
        return false;
      }
      const uint64_t SiteOffset = SiteAddress - GuestRIP;
      if (SiteOffset > std::numeric_limits<uint32_t>::max() || SiteSize > std::numeric_limits<uint32_t>::max()) {
        return false;
      }
      PatchableFields.emplace_back(static_cast<uint32_t>(SiteOffset), static_cast<uint32_t>(SiteSize));
    }
    std::sort(PatchableFields.begin(), PatchableFields.end());
    PatchableFields.erase(std::unique(PatchableFields.begin(), PatchableFields.end()), PatchableFields.end());

    fextl::vector<uint32_t> GuestExtents;
    uint64_t HashedUntil = 0;
    for (const auto& [MaskOffset, MaskSize] : PatchableFields) {
      if (MaskOffset < HashedUntil) {
        return false; // overlapping patch fields are ambiguous
      }
      if (MaskOffset > HashedUntil) {
        GuestExtents.push_back(static_cast<uint32_t>(HashedUntil));
        GuestExtents.push_back(MaskOffset - static_cast<uint32_t>(HashedUntil));
      }
      HashedUntil = static_cast<uint64_t>(MaskOffset) + MaskSize;
    }
    if (HashedUntil < GuestCode.size()) {
      GuestExtents.push_back(static_cast<uint32_t>(HashedUntil));
      GuestExtents.push_back(static_cast<uint32_t>(GuestCode.size() - HashedUntil));
    }
    if (GuestCode.empty() || GuestExtents.empty() || GuestExtents.size() > std::numeric_limits<uint32_t>::max() ||
        !ValidateGuestExtents(GuestExtents, static_cast<uint32_t>(GuestCode.size()))) {
      return false;
    }

    XXH3_state_t GuestHashState;
    XXH3_128bits_reset(&GuestHashState);
    for (size_t i = 0; i < GuestExtents.size(); i += 2) {
      XXH3_128bits_update(&GuestHashState, GuestCode.data() + GuestExtents[i], GuestExtents[i + 1]);
    }
    const XXH128_hash_t GuestHash = XXH3_128bits_digest(&GuestHashState);

    const size_t HeaderOffset = 0;
    const size_t GuestExtentsBytes = GuestExtents.size() * sizeof(uint32_t);
    const size_t HostCodeOffset = HeaderOffset + sizeof(BlobFixedHeader) + GuestExtentsBytes;
    const size_t EntryPointsBytes = static_cast<size_t>(EntryPointCount) * sizeof(BlobEntryPoint);
    const size_t SmallRelocsBytes = static_cast<size_t>(SmallRelocCount) * sizeof(BlobSmallRelocation);
    const size_t ThunkRelocsBytes = static_cast<size_t>(ThunkRelocCount) * sizeof(BlobThunkRelocation);
    const size_t GuestPagesBytes = static_cast<size_t>(TouchedGuestPagesCount) * sizeof(int64_t);
    constexpr size_t MaxSize = std::numeric_limits<size_t>::max();
    if (GuestExtentsBytes > MaxSize - sizeof(BlobFixedHeader) ||
        CompiledCode.Size > MaxSize - HostCodeOffset ||
        EntryPointsBytes > MaxSize - HostCodeOffset - CompiledCode.Size ||
        SmallRelocsBytes > MaxSize - HostCodeOffset - CompiledCode.Size - EntryPointsBytes ||
        ThunkRelocsBytes > MaxSize - HostCodeOffset - CompiledCode.Size - EntryPointsBytes - SmallRelocsBytes ||
        GuestPagesBytes > MaxSize - HostCodeOffset - CompiledCode.Size - EntryPointsBytes - SmallRelocsBytes - ThunkRelocsBytes) {
      return false;
    }
    const size_t EntryPointsOffset = HostCodeOffset + CompiledCode.Size;
    const size_t SmallRelocsOffset = EntryPointsOffset + EntryPointsBytes;
    const size_t ThunkRelocsOffset = SmallRelocsOffset + SmallRelocsBytes;
    const size_t TouchedGuestPagesOffset = ThunkRelocsOffset + ThunkRelocsBytes;
    const size_t GuestCodeOffset = TouchedGuestPagesOffset + GuestPagesBytes;
    if (GuestCode.size() > MaxSize - GuestCodeOffset ||
        GuestCodeOffset + GuestCode.size() > MaxSerializedField) {
      return false;
    }
    const size_t TotalSize = GuestCodeOffset + GuestCode.size();

    // we'll copy everything into here and pass it to the Writer, then return to caller quickly
    // Allocate the final shared backing storage before taking BlobData. This mirrors
    // upstream b49837f: moving a vector into shared ownership after taking data()
    // can leave stale pointers under -O0 when the move becomes a copy.
    auto BlobRef = fextl::make_shared<fextl::vector<uint8_t>>(TotalSize);
    uint8_t* BlobData = BlobRef->data();

    const auto BlockFootprint = MakeBlockFootprint(DecodedBlockInfo, GuestRIP);
    if (!BlockFootprint) {
      return false;
    }
    // Reuse the code identity computed during Lookup. Store runs after CompileCode
    // has released the decoder buffer, so recomputing this key here could decode
    // into an unowned buffer and double-disown it.
    const uint64_t CodeKey = GuestCodeKey;
    auto RangeInfo = CTX->SyscallHandler->QueryGuestExecutableRange(Thread, GuestRIP);
    uint64_t BlobKey = MakeBlobKey(Thread, CodeKey, *BlockFootprint, RangeInfo.Writable,
                                   GuestRIP == CTX->GetMonoBackPatcherBlock());
    MesaFOZ::foz_payload_key Key = {};
    fextl::string BlobName = fextl::fmt::format("{:016x}", BlobKey);
    memcpy(Key.bytes, BlobName.data(), BlobName.size());

    BlobFixedHeader Header {
      .GuestSize = (uint32_t)GuestCode.size(),
      .HostSize = static_cast<uint32_t>(CompiledCode.Size),
      .EntryPointCount = EntryPointCount,
      .SmallRelocCount = SmallRelocCount,
      .ThunkRelocCount = ThunkRelocCount,
      .TouchedGuestPagesCount = TouchedGuestPagesCount,
      .GuestExtentsCount = static_cast<uint32_t>(GuestExtents.size()),
      .GuestHash = GuestHash,
    };
    memcpy(BlobData + HeaderOffset, &Header, sizeof(Header));
    memcpy(BlobData + sizeof(Header), GuestExtents.data(), GuestExtentsBytes);
    memcpy(BlobData + HostCodeOffset, CompiledCode.BlockBegin, CompiledCode.Size);
    // Blob was value-initialized by resize(), so alignment padding is zero-filled.

    // pack and relocate entrypoints
    auto* EntryPoints = reinterpret_cast<BlobEntryPoint*>(BlobData + EntryPointsOffset);
    uint32_t EntryIdx = 0;
    // Match upstream: guest entrypoints are serialized relative to this block's
    // entry RIP, independent of the executable's mapping/segment base.
    for (auto [GuestAddr, HostAddr] : CompiledCode.EntryPoints) {
      if (GuestAddr < GuestRIP || GuestAddr - GuestRIP > std::numeric_limits<uint32_t>::max()) {
        return false;
      }
      EntryPoints[EntryIdx++] = {GuestAddr - GuestRIP, uint32_t(HostAddr - CompiledCode.BlockBegin)};
    }

    // pack relocations
    auto* SmallRelocs = reinterpret_cast<BlobSmallRelocation*>(BlobData + SmallRelocsOffset);
    auto* ThunkRelocs = reinterpret_cast<BlobThunkRelocation*>(BlobData + ThunkRelocsOffset);
    uint32_t SmallIdx = 0;
    uint32_t ThunkIdx = 0;
    for (const auto& Reloc : Relocations) {
      // Relocations are block-relative, so serialize their offsets directly.
      uint32_t LocalOffset = uint32_t(Reloc.Header.Offset);

      switch (Reloc.Header.Type) {
      // it's important to zero-init the element completely so we don't have garbage in unused fields
      // this way, the caches stay deterministic across machines
      case CPU::RelocationTypes::RELOC_NAMED_SYMBOL_LITERAL: {
        BlobSmallRelocation SmallReloc = {};
        SmallReloc.Offset = LocalOffset;
        SmallReloc.Type = uint8_t(Reloc.Header.Type);
        SmallReloc.Named.Symbol = uint32_t(Reloc.NamedSymbolLiteral.Symbol);
        SmallRelocs[SmallIdx++] = SmallReloc;
        break;
      }
      case CPU::RelocationTypes::RELOC_GUEST_RIP_LITERAL: {
        BlobSmallRelocation SmallReloc = {};
        SmallReloc.Offset = LocalOffset;
        SmallReloc.Type = uint8_t(Reloc.Header.Type);
        SmallReloc.RIPLiteral.GuestRIP = Reloc.GuestRIP.GuestRIP - GuestRIP;
        SmallRelocs[SmallIdx++] = SmallReloc;
        break;
      }
      case CPU::RelocationTypes::RELOC_GUEST_RIP_MOVE: {
        BlobSmallRelocation SmallReloc = {};
        SmallReloc.Offset = LocalOffset;
        SmallReloc.Type = uint8_t(Reloc.Header.Type);
        SmallReloc.RIPMove.RegisterIndex = Reloc.GuestRIP.RegisterIndex;
        SmallReloc.RIPMove.GuestRIP = Reloc.GuestRIP.GuestRIP - GuestRIP;
        SmallRelocs[SmallIdx++] = SmallReloc;
        break;
      }
      case CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_DATA_MOVE:
      case CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_CRC_MOVE:
      case CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_LITERAL:
      case CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_MOVE: {
        BlobSmallRelocation SmallReloc = {};
        SmallReloc.Offset = LocalOffset;
        SmallReloc.Type = uint8_t(Reloc.Header.Type);
        SmallReloc.PatchableData.RegisterIndex = Reloc.GuestPatchableData.RegisterIndex;
        SmallReloc.PatchableData.ValueSize = Reloc.GuestPatchableData.ValueSize;
        SmallReloc.PatchableData.SignExtend = Reloc.GuestPatchableData.SignExtend ? 1 : 0;
        SmallReloc.PatchableData.SiteOffset = static_cast<uint32_t>(Reloc.GuestPatchableData.SiteAddress - GuestRIP);
        SmallRelocs[SmallIdx++] = SmallReloc;
        break;
      }
      case CPU::RelocationTypes::RELOC_NAMED_THUNK_MOVE: {
        BlobThunkRelocation BigReloc = {};
        BigReloc.Offset = LocalOffset;
        BigReloc.RegisterIndex = Reloc.NamedThunkMove.RegisterIndex;
        memcpy(BigReloc.SymbolHash, &Reloc.NamedThunkMove.Symbol, sizeof(BigReloc.SymbolHash));
        ThunkRelocs[ThunkIdx++] = BigReloc;
        break;
      }
      }
    }

    // relocate touched pages relative to GuestRIP
    // in theory we could save some size here, unlikely we need all 64bits
    auto* PageOffsets = reinterpret_cast<int64_t*>(BlobData + TouchedGuestPagesOffset);
    uint32_t PageIdx = 0;
    for (auto GuestPage : DecodedBlockInfo->CodePages) {
      PageOffsets[PageIdx++] = GuestPage - GuestRIP;
    }

    memcpy(BlobData + GuestCodeOffset, GuestCode.data(), GuestCode.size());

    // Buffer queued writes only within the configured memory budget. If disabled or full,
    // store synchronously rather than growing an unbounded queue of cache blobs.
    uint64_t ReservedBytes = 0;
    if (MaxWriteBufferBytes && BlobRef->size() <= MaxWriteBufferBytes) {
      uint64_t Pending = PendingWriteBufferBytes.load(std::memory_order_relaxed);
      while (Pending <= MaxWriteBufferBytes - BlobRef->size()) {
        if (PendingWriteBufferBytes.compare_exchange_weak(Pending, Pending + BlobRef->size(), std::memory_order_relaxed)) {
          ReservedBytes = BlobRef->size();
          break;
        }
      }
    }

    if (ReservedBytes) {
      auto Work = fextl::make_unique<CacheStoreWorkItem>(this, RWCacheDB.get(), Key, BlobRef, ReservedBytes);
      if (Writer) {
        Writer->QueueWork(std::move(Work));
      } else {
        // If the worker queue couldn't be allocated, preserve cache correctness by
        // writing synchronously. Work-item destruction releases the buffer reservation.
        Work->Run();
      }
    } else {
      RWCacheDB->StoreCacheBlob(Key, *BlobRef, Index, IndexLock);
    }
    return true;
  }

} // namespace DiskCache

} // namespace FEXCore
