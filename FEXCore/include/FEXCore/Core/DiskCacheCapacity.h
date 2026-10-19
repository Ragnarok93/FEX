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

// Keep capacity admission boundary and overflow behavior checked in every build.
static_assert(CanStoreWithinCapacity(100, 40, 20, 30, 10));
static_assert(!CanStoreWithinCapacity(100, 40, 20, 30, 11));
static_assert(!CanStoreWithinCapacity(100, 101, 0, 0, 0));
static_assert(CanStoreWithinCapacity(0, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX));
static_assert(!CanStoreWithinCapacity(UINT64_MAX, UINT64_MAX - 1, 0, 2, 0));

} // namespace FEXCore::DiskCache
