// SPDX-License-Identifier: MIT

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
      FD->Truncate(static_cast<uint64_t>(FileSize));
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
    return true;
  }

  void IndexedDB::PopulateIndex(Index& CacheIndex, bool& FoundMetadata) {
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
      if (FOZKey->bytes[39] != 0xFF) {
        uint64_t Key {};
        const char* KeyBegin = reinterpret_cast<const char*>(FOZKey->bytes);
        const char* KeyEnd = reinterpret_cast<const char*>(&FOZKey->bytes[16]);
        const auto ParseResult = std::from_chars(KeyBegin, KeyEnd, Key, 16);
        // Cache files persist across runs and can be truncated or corrupted. Never use
        // an uninitialized hash when a malformed key fails integer parsing.
        if (ParseResult.ec != std::errc {} || ParseResult.ptr != KeyEnd || IndexBlobPayload->hash != Key) {
          continue;
        }
        CacheIndex.insert({IndexBlobPayload->hash, {this, IndexBlobPayload->cache_db_file_offset, IndexBlobPayload->size}});
      } else {
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
    uint64_t Hash {};
    if (Key.bytes[39] != 0xFF) {
      const char* KeyBegin = reinterpret_cast<const char*>(Key.bytes);
      const char* KeyEnd = reinterpret_cast<const char*>(&Key.bytes[16]);
      const auto ParseResult = std::from_chars(KeyBegin, KeyEnd, Hash, 16);
      if (ParseResult.ec != std::errc {} || ParseResult.ptr != KeyEnd) {
        return false;
      }
    } else {
      Hash = ~0;
    }
    {
      std::lock_guard Guard(IndexMutex);
      if (Index.contains(Hash)) {
        // shouldn't really happen.. assert or something?
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
      if (Index.contains(Hash)) {
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
    if (!CanStoreWithinCapacity(MaxSizeBytes, static_cast<uint64_t>(CacheBytes), static_cast<uint64_t>(IndexBytes),
                                PendingCacheBytes, PendingIndexBytes)) {
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
      CacheFOZ.Truncate(static_cast<uint64_t>(CacheBytes));
      CacheFOZ.Unlock();
      IndexFOZ.Unlock();
      return false;
    }

    CacheFOZ.Unlock();
    IndexFOZ.Unlock();

    // publish new file size for memory-mapped reads
    if (CacheFileMapping) {
      CacheFileSize = BlobOffset + Blob.size();
    }

    std::lock_guard Guard(IndexMutex);
    Index[Hash] = {this, BlobOffset, (uint32_t)Blob.size()};
    return true;
  }

  bool DiskCache::OpenCacheDB(const fextl::string& CacheDBName, bool ReadOnly) {
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

    CurDB->PopulateIndex(Index, FoundMetadata);

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

  void DiskCache::Init(FEXCore::Context::ContextImpl* CTX) {
    this->CTX = CTX;
    MaxWriteBufferBytes = std::min<uint64_t>(static_cast<uint64_t>(MemoryBufferMB()) * 1024ULL * 1024ULL, 1ULL << 30);

    if (!EnableDiskCache) {
      return;
    }

    fextl::string SerializedConfig = FEXCore::Config::SerializeForCache();

    struct __attribute__((packed)) {
      uint8_t FormatVersion;
      uint8_t Is64BitMode;
      uint64_t HostFeaturesHash;
    } BucketHeader = {FormatVersion, CTX->Config.Is64BitMode, CTX->HostFeatures.HashForCaching()};

    fextl::vector<uint8_t> BucketBytes;
    BucketBytes.resize(sizeof(BucketHeader) + SerializedConfig.size());
    memcpy(BucketBytes.data(), &BucketHeader, sizeof(BucketHeader));
    memcpy(BucketBytes.data() + sizeof(BucketHeader), SerializedConfig.data(), SerializedConfig.size());
    BucketHash = XXH3_128bits(BucketBytes.data(), BucketBytes.size());

    fextl::string BasePath = BasePathOverride();
    if (BasePath.empty()) {
      BasePath = FEXCore::Config::GetCacheDirectory() + "DiskCache/";
      BasePath += fextl::fmt::format("{:016x}{:016x}", BucketHash.high64, BucketHash.low64) + "/";
    }
    FHU::Filesystem::CreateDirectories(BasePath);

    if (!MapDiskCacheFiles) {
      FileMapper = nullptr;
    }

    fextl::string RWDBBasePath = BasePath + "RWCacheDB";
    OpenCacheDB(RWDBBasePath, false);

    if (RWCacheDB && !FoundMetadata) {
      // we just opened a fresh cache, add a metadata blob
      MesaFOZ::foz_payload_key MetadataKey;
      memset(MetadataKey.bytes, 0xFF, sizeof(MetadataKey));
      RWCacheDB->StoreCacheBlob(MetadataKey, {BucketBytes.data(), BucketBytes.size()}, Index, IndexLock);
      Index.clear();
    }

    std::string_view RONames = RODBNames();
    while (!RONames.empty()) {
      const auto Delim = RONames.find(',');
      const std::string_view ROName = RONames.substr(0, Delim);
      if (!ROName.empty()) {
        fextl::string RODBBasePath = BasePath;
        RODBBasePath += ROName;
        OpenCacheDB(RODBBasePath, true);
      }
      if (Delim == std::string_view::npos) {
        break;
      }
      // advance to next
      RONames.remove_prefix(Delim + 1);
    }

    if (IsWritingDiskCache()) {
      Writer = fextl::make_unique<WorkQueueThread>();
    }
  }

  uint64_t DiskCache::MakeBlobKey(const uint64_t ModuleOffset) {
    struct {
      uint64_t ModuleOffset;
      XXH128_hash_t BucketHash;
    } BlobKeyBytes = {ModuleOffset, BucketHash};

    return XXH3_64bits(&BlobKeyBytes, sizeof(BlobKeyBytes));
  }

  std::optional<CodeHitData> DiskCache::Lookup(Core::InternalThreadState* Thread, const ExecutableFileSectionInfo& Region, uint64_t GuestRIP) {
    if (!IsReadingDiskCache()) {
      return std::nullopt;
    }
    uint64_t ModuleOffset = GuestRIP - Region.FileStartVA;

    uint64_t Hash = MakeBlobKey(ModuleOffset);

    IndexEntry Entry;
    {
      std::lock_guard Guard(IndexLock);
      auto It = Index.find(Hash);
      if (It == Index.end()) {
        // definite miss
        return std::nullopt;
      }
      // we can't hold onto the iterator, the map may shift while we don't hold the lock
      Entry = It->second;
    }
    // found a key hash match, could still be a miss, read the blob and verify more
    CodeHitData HitData;
    HitData.Blob.resize(Entry.Size);
    if (!Entry.DB->ReadCacheBlob(Entry.Offset, HitData.Blob)) {
      return std::nullopt;
    }

    if (Entry.Size < sizeof(BlobFixedHeader)) {
      return std::nullopt;
    }
    BlobFixedHeader Header;
    memcpy(&Header, HitData.Blob.data(), sizeof(Header));

    // do we have enough room in our live code to even hash GuestSize worth?
    auto RangeInfo = CTX->SyscallHandler->QueryGuestExecutableRange(Thread, GuestRIP);
    if (RangeInfo.Size == 0 || RangeInfo.Base > GuestRIP) {
      return std::nullopt;
    }
    const uint64_t GuestOffsetInRange = GuestRIP - RangeInfo.Base;
    if (GuestOffsetInRange >= RangeInfo.Size) {
      return std::nullopt;
    }
    const uint64_t Available = RangeInfo.Size - GuestOffsetInRange;
    if (Available < Header.GuestSize) {
      return std::nullopt;
    }

    XXH128_hash_t LiveGuestHash = XXH3_128bits(reinterpret_cast<void*>(GuestRIP), Header.GuestSize);
    if (std::memcmp(&LiveGuestHash, &Header.GuestHash, sizeof(Header.GuestHash)) != 0) {
      // LogMan::Msg::IFmt("hash mismatch! length {:d}", Header.GuestSize);
      return std::nullopt;
    }
    // LogMan::Msg::IFmt("hash ok! length {:d}", Header.GuestSize);

    // Validate the serialized lengths using 64-bit arithmetic. The cache is persistent
    // input, so a corrupt header must not wrap SizeNeeded and produce out-of-bounds spans.
    const uint64_t SizeNeeded = sizeof(Header) + static_cast<uint64_t>(Header.HostSize) +
                                static_cast<uint64_t>(Header.EntryPointCount) * sizeof(BlobEntryPoint) +
                                static_cast<uint64_t>(Header.SmallRelocCount) * sizeof(BlobSmallRelocation) +
                                static_cast<uint64_t>(Header.ThunkRelocCount) * sizeof(BlobThunkRelocation) +
                                static_cast<uint64_t>(Header.TouchedGuestPagesCount) * sizeof(int64_t);
    if (SizeNeeded > Entry.Size) {
      return std::nullopt;
    }

    HitData.HostCode = {HitData.Blob.data() + sizeof(Header), Header.HostSize};
    HitData.EntryPoints = {reinterpret_cast<const BlobEntryPoint*>(HitData.Blob.data() + sizeof(Header) + Header.HostSize), Header.EntryPointCount};

    auto* SmallRelocs = reinterpret_cast<const BlobSmallRelocation*>(
      HitData.Blob.data() + sizeof(Header) + Header.HostSize + Header.EntryPointCount * sizeof(BlobEntryPoint));
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
      case uint8_t(CPU::RelocationTypes::RELOC_GUEST_RIP_LITERAL): Reloc.GuestRIP.GuestRIP = SmallReloc.RIPLiteral.GuestRIP; break;
      case uint8_t(CPU::RelocationTypes::RELOC_GUEST_RIP_MOVE):
        Reloc.GuestRIP.RegisterIndex = SmallReloc.RIPMove.RegisterIndex;
        Reloc.GuestRIP.GuestRIP = SmallReloc.RIPMove.GuestRIP;
        break;
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
      HitData.GuestPages.push_back(GuestRIP + PageOffsets[i]);
    }

    return HitData;
  }

  static inline bool IsRelocationInBlock(const FEXCore::CPU::Relocation& Reloc, const CPU::CPUBackend::CompiledCode& CompiledCode) {
    return Reloc.Header.Offset >= CompiledCode.HostCodeOffset && Reloc.Header.Offset < CompiledCode.HostCodeOffset + CompiledCode.Size;
  }

  struct DiskCache::CacheStoreWorkItem final : WorkQueueThread::WorkItem {
    DiskCache* Self;
    IndexedDB* DB;
    MesaFOZ::foz_payload_key Key;
    fextl::vector<uint8_t> Blob;
    uint64_t ReservedBytes {};
    CacheStoreWorkItem(DiskCache* Self, IndexedDB* DB, const MesaFOZ::foz_payload_key& Key, fextl::vector<uint8_t>&& Blob, uint64_t ReservedBytes)
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
      DB->StoreCacheBlob(Key, Blob, Self->Index, Self->IndexLock);
    }
  };

  bool DiskCache::Store(Core::InternalThreadState* Thread, const ExecutableFileSectionInfo& Region, uint64_t GuestRIP,
                        std::span<const uint8_t> GuestCode, const CPU::CPUBackend::CompiledCode& CompiledCode,
                        std::span<const FEXCore::CPU::Relocation> Relocations, const Frontend::Decoder::DecodedBlockInformation* DecodedBlockInfo) {
    if (!IsWritingDiskCache()) {
      return false;
    }
    if (!DecodedBlockInfo) {
      return false;
    }

    // check for any reloc targets outside of our jurisdiction
    // todo what are they exactly? caching those blocks is great when it works, so need to figure this out and make finer-grained if we can
    if (RelocationFilter) {
      for (const auto& Reloc : Relocations) {
        if (!IsRelocationInBlock(Reloc, CompiledCode)) {
          continue;
        }
        if (Reloc.Header.Type != CPU::RelocationTypes::RELOC_GUEST_RIP_LITERAL && Reloc.Header.Type != CPU::RelocationTypes::RELOC_GUEST_RIP_MOVE) {
          continue;
        }
        uint64_t Target = Reloc.GuestRIP.GuestRIP;
        if (Target >= Region.BeginVA && Target < Region.EndVA) {
          continue;
        }
        auto TargetSection = CTX->SyscallHandler->LookupExecutableFileSection(Thread, Target);
        if (!TargetSection || TargetSection->FileInfo.FileId != Region.FileInfo.FileId) {
          // we don't know where it's pointing, so we don't know how to encode the offset, so we can't cache atm
          return false;
        }
      }
    }

    uint32_t SmallRelocCount = 0;
    uint32_t ThunkRelocCount = 0;
    for (const auto& Reloc : Relocations) {
      // relocs aren't cleared every time if IsGeneratingCache, so filter just in case
      if (!IsRelocationInBlock(Reloc, CompiledCode)) {
        continue;
      }
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

    const size_t HeaderOffset = 0;
    const size_t HostCodeOffset = HeaderOffset + sizeof(BlobFixedHeader);
    const size_t EntryPointsBytes = static_cast<size_t>(EntryPointCount) * sizeof(BlobEntryPoint);
    const size_t SmallRelocsBytes = static_cast<size_t>(SmallRelocCount) * sizeof(BlobSmallRelocation);
    const size_t ThunkRelocsBytes = static_cast<size_t>(ThunkRelocCount) * sizeof(BlobThunkRelocation);
    const size_t GuestPagesBytes = static_cast<size_t>(TouchedGuestPagesCount) * sizeof(int64_t);
    constexpr size_t MaxSize = std::numeric_limits<size_t>::max();
    if (CompiledCode.Size > MaxSize - HostCodeOffset ||
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
    fextl::vector<uint8_t> Blob;
    Blob.resize(TotalSize);
    uint8_t* BlobData = Blob.data();

    uint64_t ModuleOffset = GuestRIP - Region.FileStartVA;

    uint64_t BlobKey = MakeBlobKey(ModuleOffset);
    MesaFOZ::foz_payload_key Key = {};
    fextl::string BlobName = fextl::fmt::format("{:016x}", BlobKey);
    memcpy(Key.bytes, BlobName.data(), BlobName.size());

    BlobFixedHeader Header {
      .GuestSize = (uint32_t)GuestCode.size(),
      .HostSize = (uint32_t)CompiledCode.Size,
      .EntryPointCount = EntryPointCount,
      .SmallRelocCount = SmallRelocCount,
      .ThunkRelocCount = ThunkRelocCount,
      .TouchedGuestPagesCount = TouchedGuestPagesCount,
      .GuestHash = XXH3_128bits(GuestCode.data(), GuestCode.size()),
    };
    memcpy(BlobData + HeaderOffset, &Header, sizeof(Header));
    memcpy(BlobData + HostCodeOffset, CompiledCode.BlockBegin, CompiledCode.Size);

    // pack and relocate entrypoints
    auto* EntryPoints = reinterpret_cast<BlobEntryPoint*>(BlobData + EntryPointsOffset);
    uint32_t EntryIdx = 0;
    for (auto [GuestAddr, HostAddr] : CompiledCode.EntryPoints) {
      EntryPoints[EntryIdx++] = {GuestAddr - Region.FileStartVA, uint32_t(HostAddr - CompiledCode.BlockBegin)};
    }

    // pack relocations
    auto* SmallRelocs = reinterpret_cast<BlobSmallRelocation*>(BlobData + SmallRelocsOffset);
    auto* ThunkRelocs = reinterpret_cast<BlobThunkRelocation*>(BlobData + ThunkRelocsOffset);
    uint32_t SmallIdx = 0;
    uint32_t ThunkIdx = 0;
    for (const auto& Reloc : Relocations) {
      if (!IsRelocationInBlock(Reloc, CompiledCode)) {
        continue;
      }
      // re-relocate :harold:
      uint32_t LocalOffset = uint32_t(Reloc.Header.Offset - CompiledCode.HostCodeOffset);

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
    if (MaxWriteBufferBytes && Blob.size() <= MaxWriteBufferBytes) {
      uint64_t Pending = PendingWriteBufferBytes.load(std::memory_order_relaxed);
      while (Pending <= MaxWriteBufferBytes - Blob.size()) {
        if (PendingWriteBufferBytes.compare_exchange_weak(Pending, Pending + Blob.size(), std::memory_order_relaxed)) {
          ReservedBytes = Blob.size();
          break;
        }
      }
    }

    if (ReservedBytes) {
      auto Work = fextl::make_unique<CacheStoreWorkItem>(this, RWCacheDB.get(), Key, std::move(Blob), ReservedBytes);
      if (Writer) {
        Writer->QueueWork(std::move(Work));
      } else {
        // If the worker queue couldn't be allocated, preserve cache correctness by
        // writing synchronously. Work-item destruction releases the buffer reservation.
        Work->Run();
      }
    } else {
      RWCacheDB->StoreCacheBlob(Key, Blob, Index, IndexLock);
    }
    return true;
  }

} // namespace DiskCache

} // namespace FEXCore
