#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys


def fail(message: str) -> None:
    raise SystemExit(message)


def replace_once(root: pathlib.Path, relative: str, old: str, new: str) -> None:
    path = root / relative
    data = path.read_text()
    count = data.count(old)
    if count != 1:
        fail(f"{relative}: expected exactly one patch site, found {count}")
    path.write_text(data.replace(old, new, 1))


def main() -> None:
    if len(sys.argv) != 2:
        fail(f"Usage: {sys.argv[0]} <exact-fex-2609-source-dir>")

    root = pathlib.Path(sys.argv[1]).resolve()
    if not (root / "FEXCore/Source/Interface/Core/DiskCache.cpp").is_file():
        fail(f"Not a FEX source tree: {root}")

    replace_once(
        root,
        "FEXCore/Source/CMakeLists.txt",
        """# Generate config
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Interface/Config/Config.json.in
  ${CMAKE_BINARY_DIR}/generated/Config/Config.json)
""",
        """# Generate config
# Keep these as generated FEX defaults rather than forcing policy in the runtime.
# Normal FEX JSON/environment layers can still override either value.
set(FEX_DISKCACHE_DEFAULT \"false\" CACHE STRING \"Default state for FEX persistent disk cache\")
set_property(CACHE FEX_DISKCACHE_DEFAULT PROPERTY STRINGS false true)
if (NOT FEX_DISKCACHE_DEFAULT STREQUAL \"false\" AND NOT FEX_DISKCACHE_DEFAULT STREQUAL \"true\")
  message(FATAL_ERROR \"FEX_DISKCACHE_DEFAULT must be 'false' or 'true'\")
endif()

set(FEX_DISKCACHE_MAX_SIZE_MB_DEFAULT \"0\" CACHE STRING \"Default writable FEX disk-cache capacity in MiB; 0 is unlimited\")
if (NOT FEX_DISKCACHE_MAX_SIZE_MB_DEFAULT MATCHES \"^[0-9]+$\")
  message(FATAL_ERROR \"FEX_DISKCACHE_MAX_SIZE_MB_DEFAULT must be an unsigned integer\")
endif()
if (FEX_DISKCACHE_MAX_SIZE_MB_DEFAULT GREATER 4294967295)
  message(FATAL_ERROR \"FEX_DISKCACHE_MAX_SIZE_MB_DEFAULT exceeds uint32 range\")
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Interface/Config/Config.json.in
  ${CMAKE_BINARY_DIR}/generated/Config/Config.json)
""",
    )

    replace_once(
        root,
        "FEXCore/Source/Interface/Config/Config.json.in",
        """      \"DiskCache\": {
        \"Type\": \"bool\",
        \"Default\": \"false\",
        \"AffectsCodeGen\": \"false\",
        \"Desc\": [
          \"Enables disk caching for code blocks\"
        ]
      },
      \"DiskCacheFileMapping\": {
""",
        """      \"DiskCache\": {
        \"Type\": \"bool\",
        \"Default\": \"@FEX_DISKCACHE_DEFAULT@\",
        \"AffectsCodeGen\": \"false\",
        \"Desc\": [
          \"Enables disk caching for code blocks\"
        ]
      },
      \"DiskCacheMaxSizeMB\": {
        \"Type\": \"uint32\",
        \"Default\": \"@FEX_DISKCACHE_MAX_SIZE_MB_DEFAULT@\",
        \"AffectsCodeGen\": \"false\",
        \"Desc\": [
          \"Maximum total size in MiB of the writable disk-cache database and its index.\",
          \"When the limit is reached, existing entries remain readable and new entries are not admitted.\",
          \"A value of 0 keeps upstream unlimited behaviour.\"
        ]
      },
      \"DiskCacheFileMapping\": {
""",
    )

    capacity_header = root / "FEXCore/include/FEXCore/Core/DiskCacheCapacity.h"
    if capacity_header.exists():
        fail(f"Refusing to overwrite existing file: {capacity_header}")
    capacity_header.write_text(
        """// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace FEXCore::DiskCache {

// A zero limit preserves upstream's unlimited cache behaviour. For bounded
// caches, consume the available budget one component at a time so no addition
// can overflow while checking the total writable database + index footprint.
constexpr bool CanStoreWithinCapacity(uint64_t MaxBytes, uint64_t CurrentCacheBytes, uint64_t CurrentIndexBytes,
                                      uint64_t PendingCacheBytes, uint64_t PendingIndexBytes) {
  if (MaxBytes == 0) {
    return true;
  }

  uint64_t Remaining = MaxBytes;
  if (CurrentCacheBytes > Remaining) {
    return false;
  }
  Remaining -= CurrentCacheBytes;
  if (CurrentIndexBytes > Remaining) {
    return false;
  }
  Remaining -= CurrentIndexBytes;
  if (PendingCacheBytes > Remaining) {
    return false;
  }
  Remaining -= PendingCacheBytes;
  return PendingIndexBytes <= Remaining;
}

} // namespace FEXCore::DiskCache
"""
    )

    replace_once(
        root,
        "FEXCore/include/FEXCore/Core/DiskCache.h",
        """    bool Open(const fextl::string& CacheDBName, bool ReadOnly);
""",
        """    bool Open(const fextl::string& CacheDBName, bool ReadOnly, uint64_t MaxSizeBytes = 0);
""",
    )
    replace_once(
        root,
        "FEXCore/include/FEXCore/Core/DiskCache.h",
        """    FOZFile IndexFOZ;
    bool ReadOnly = false;
""",
        """    FOZFile IndexFOZ;
    bool ReadOnly = false;
    uint64_t MaxSizeBytes = 0;
""",
    )
    replace_once(
        root,
        "FEXCore/include/FEXCore/Core/DiskCache.h",
        """    bool OpenCacheDB(const fextl::string& CacheDBName, bool ReadOnly);
""",
        """    bool OpenCacheDB(const fextl::string& CacheDBName, bool ReadOnly, uint64_t MaxSizeBytes = 0);
""",
    )
    replace_once(
        root,
        "FEXCore/include/FEXCore/Core/DiskCache.h",
        """    FEX_CONFIG_OPT(EnableDiskCache, DISKCACHE);
    FEX_CONFIG_OPT(Validation, DISKCACHEVALIDATION);
""",
        """    FEX_CONFIG_OPT(EnableDiskCache, DISKCACHE);
    FEX_CONFIG_OPT(MaxSizeMB, DISKCACHEMAXSIZEMB);
    FEX_CONFIG_OPT(Validation, DISKCACHEVALIDATION);
""",
    )

    replace_once(
        root,
        "FEXCore/Source/Interface/Core/DiskCache.cpp",
        """#include \"FEXCore/Core/DiskCache.h\"
#include \"FEXCore/Core/DiskCacheFileMapper.h\"
""",
        """#include \"FEXCore/Core/DiskCache.h\"
#include \"FEXCore/Core/DiskCacheCapacity.h\"
#include \"FEXCore/Core/DiskCacheFileMapper.h\"
""",
    )
    replace_once(
        root,
        "FEXCore/Source/Interface/Core/DiskCache.cpp",
        """#include <cstdint>
#include <cstring>
#include <atomic>
""",
        """#include <cstdint>
#include <cstring>
#include <atomic>
#include <limits>
""",
    )
    replace_once(
        root,
        "FEXCore/Source/Interface/Core/DiskCache.cpp",
        """  bool IndexedDB::Open(const fextl::string& CacheDBName, bool ReadOnly) {
""",
        """  bool IndexedDB::Open(const fextl::string& CacheDBName, bool ReadOnly, uint64_t MaxSizeBytes) {
""",
    )
    replace_once(
        root,
        "FEXCore/Source/Interface/Core/DiskCache.cpp",
        """    this->ReadOnly = ReadOnly;
    return true;
  }

  void IndexedDB::PopulateIndex""",
        """    this->ReadOnly = ReadOnly;
    this->MaxSizeBytes = ReadOnly ? 0 : MaxSizeBytes;
    return true;
  }

  void IndexedDB::PopulateIndex""",
    )
    replace_once(
        root,
        "FEXCore/Source/Interface/Core/DiskCache.cpp",
        """    if (!CacheFOZ.Lock(STORE_LOCK_TIMEOUT_MS) || !IndexFOZ.Lock(STORE_LOCK_TIMEOUT_MS)) {
      CacheFOZ.Unlock();
      IndexFOZ.Unlock();
      return false;
    }

    // write cache side first so we get offset for index
""",
        """    if (!CacheFOZ.Lock(STORE_LOCK_TIMEOUT_MS) || !IndexFOZ.Lock(STORE_LOCK_TIMEOUT_MS)) {
      CacheFOZ.Unlock();
      IndexFOZ.Unlock();
      return false;
    }

    if (MaxSizeBytes != 0) {
      const ssize_t CacheBytes = CacheFOZ.Size();
      const ssize_t IndexBytes = IndexFOZ.Size();
      constexpr uint64_t FOZRecordOverhead = sizeof(MesaFOZ::foz_payload_key) + sizeof(MesaFOZ::foz_payload_header);
      constexpr uint64_t IndexEntryBytes = sizeof(MesaFOZ::mesa_index_db_file_entry);
      constexpr uint64_t U64Max = std::numeric_limits<uint64_t>::max();

      if (CacheBytes < 0 || IndexBytes < 0 || Blob.size() > U64Max - FOZRecordOverhead ||
          IndexBlob.size() > U64Max - FOZRecordOverhead - IndexEntryBytes) {
        CacheFOZ.Unlock();
        IndexFOZ.Unlock();
        return false;
      }

      const uint64_t PendingCacheBytes = FOZRecordOverhead + static_cast<uint64_t>(Blob.size());
      const uint64_t PendingIndexBytes = FOZRecordOverhead + IndexEntryBytes + static_cast<uint64_t>(IndexBlob.size());
      if (!CanStoreWithinCapacity(MaxSizeBytes, static_cast<uint64_t>(CacheBytes), static_cast<uint64_t>(IndexBytes), PendingCacheBytes,
                                  PendingIndexBytes)) {
        CacheFOZ.Unlock();
        IndexFOZ.Unlock();
        // Capacity exhaustion is a cache miss policy decision, not an execution failure.
        return true;
      }
    }

    // write cache side first so we get offset for index
""",
    )
    replace_once(
        root,
        "FEXCore/Source/Interface/Core/DiskCache.cpp",
        """  bool DiskCache::OpenCacheDB(const fextl::string& CacheDBName, bool ReadOnly) {
""",
        """  bool DiskCache::OpenCacheDB(const fextl::string& CacheDBName, bool ReadOnly, uint64_t MaxSizeBytes) {
""",
    )
    replace_once(
        root,
        "FEXCore/Source/Interface/Core/DiskCache.cpp",
        """    if (!CurDB->Open(CacheDBName, ReadOnly)) {
""",
        """    if (!CurDB->Open(CacheDBName, ReadOnly, MaxSizeBytes)) {
""",
    )
    replace_once(
        root,
        "FEXCore/Source/Interface/Core/DiskCache.cpp",
        """    fextl::string RWDBBasePath = BasePath + \"RWCacheDB\";
    OpenCacheDB(RWDBBasePath, false);
""",
        """    fextl::string RWDBBasePath = BasePath + \"RWCacheDB\";
    const uint64_t MaxSizeBytes = static_cast<uint64_t>(MaxSizeMB()) * 1024ULL * 1024ULL;
    OpenCacheDB(RWDBBasePath, false, MaxSizeBytes);
""",
    )

    print("Applied bounded disk-cache patch to exact FEX 2609 source tree")


if __name__ == "__main__":
    main()
