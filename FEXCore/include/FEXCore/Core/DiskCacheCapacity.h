// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace FEXCore::DiskCache {

constexpr bool CanStoreWithinCapacity(uint64_t MaxBytes, uint64_t CurrentCacheBytes, uint64_t CurrentIndexBytes,
                                      uint64_t PendingCacheBytes, uint64_t PendingIndexBytes) {
  if (MaxBytes == 0) return true;
  uint64_t Remaining = MaxBytes;
  if (CurrentCacheBytes > Remaining) return false;
  Remaining -= CurrentCacheBytes;
  if (CurrentIndexBytes > Remaining) return false;
  Remaining -= CurrentIndexBytes;
  if (PendingCacheBytes > Remaining) return false;
  Remaining -= PendingCacheBytes;
  return PendingIndexBytes <= Remaining;
}

} // namespace FEXCore::DiskCache
