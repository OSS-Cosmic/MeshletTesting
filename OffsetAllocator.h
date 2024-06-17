// (C) Sebastian Aaltonen 2023
// MIT License (see file: LICENSE)

#pragma once

#include "Common_3/Application/Config.h"
#include "Common_3/Utilities/Interfaces/ILog.h"
#include "Common_3/Utilities/Interfaces/IMemory.h"


#define OFFSET_ALLOC_NUM_TOP_BINS 32
#define OFFSET_ALLOC_BINS_PER_LEAF 8
#define OFFSET_ALLOC_TOP_BINS_INDEX_SHIFT 3
#define OFFSET_ALLOC_LEAF_BINS_INDEX_MASK 0x7
#define OFFSET_ALLOC_NUM_LEAF_BINS  OFFSET_ALLOC_NUM_TOP_BINS *OFFSET_ALLOC_BINS_PER_LEAF

#define OFFSET_ALLOC_NO_SPACE 0xffffffff


struct OffsetAllocReport {
  uint32_t totalFreeSpace;
  uint32_t largestFreeRegion;
};

struct OffsetAllocReportFull {
  struct Region {
    uint32_t size;
    uint32_t count;
  };

  Region freeRegions[OFFSET_ALLOC_NUM_LEAF_BINS];
};

struct OffsetAllocNode {
  uint32_t dataOffset;
  uint32_t dataSize;
  uint32_t binListPrev;
  uint32_t binListNext;
  uint32_t neighborPrev;
  uint32_t neighborNext;
  bool used;
};

struct OffsetAllocation {
    uint32_t offset;
    uint32_t metadata; // internal: node index
};

struct OffsetAlloc {

  uint32_t m_size;
  uint32_t m_maxAllocs;
  uint32_t m_freeStorage;

  uint32_t m_usedBinsTop;
  uint8_t m_usedBins[OFFSET_ALLOC_NUM_TOP_BINS];
  uint32_t m_binIndices[OFFSET_ALLOC_NUM_LEAF_BINS];

  struct OffsetAllocNode *m_nodes;
  uint32_t* m_freeNodes;
  uint32_t m_freeOffset;
};

void resetOffsetAlloc(struct OffsetAlloc *alloc);
struct OffsetAllocReport offsetAllocStorageReport(struct OffsetAlloc *alloc);
struct OffsetAllocReportFull offsetAllocStorageReportFull(struct OffsetAlloc *alloc);

struct OffsetAllocation offsetAllocAlloc(struct OffsetAlloc *alloc, uint32_t size);
void offsetAllocFree(struct OffsetAlloc *alloc, struct OffsetAllocation allocation);
static inline uint32_t offsetAllocSizeAlloc(struct OffsetAlloc *alloc, struct OffsetAllocation allocation) {
  if (allocation.metadata == OFFSET_ALLOC_NO_SPACE)
    return 0;
  if (!alloc->m_nodes)
    return 0;

  return alloc->m_nodes[allocation.metadata].dataSize;
}


static inline void removeOffsetAllocator(struct OffsetAlloc *alloc) {
  tf_free(alloc->m_nodes);
  tf_free(alloc->m_freeNodes);
}
static inline void addOffsetAllocator(struct OffsetAlloc *alloc, uint32_t size,
                                      uint32_t maxAllocs = 128 * 1024) {
  memset(alloc, 0, sizeof(OffsetAlloc));
  alloc->m_size = size;
  alloc->m_maxAllocs = maxAllocs;
  resetOffsetAlloc(alloc);
}
