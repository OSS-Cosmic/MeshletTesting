#include "OffsetAllocator.h"

#define MANTISSA_BITS 3
#define MANTISSA_VALUE (1 << MANTISSA_BITS)
#define MANTISSA_MASK (MANTISSA_VALUE - 1)
#define OFFSET_ALLOC_INVALID_INDEX  0xffffffff

static inline uint32_t insertNodeIntoBin(struct OffsetAlloc *alloc,
                                         uint32_t size, uint32_t dataOffset);
static inline void removeNodeFromBin(struct OffsetAlloc *alloc, uint32_t nodeIndex); 

static inline uint32_t lzcnt_nonzero(uint32_t v) {
#ifdef _MSC_VER
  unsigned long retVal;
  _BitScanReverse(&retVal, v);
  return 31 - retVal;
#else
  return __builtin_clz(v);
#endif
}

static inline uint32_t tzcnt_nonzero(uint32_t v) {
#ifdef _MSC_VER
  unsigned long retVal;
  _BitScanForward(&retVal, v);
  return retVal;
#else
  return __builtin_ctz(v);
#endif
}
// Bin sizes follow floating point (exponent + mantissa) distribution (piecewise
// linear log approx) This ensures that for each size class, the average
// overhead percentage stays the same
uint32_t uintToFloatRoundUp(uint32_t size) {
  uint32_t exp = 0;
  uint32_t mantissa = 0;

  if (size < MANTISSA_VALUE) {
    // Denorm: 0..(MANTISSA_VALUE-1)
    mantissa = size;
  } else {
    // Normalized: Hidden high bit always 1. Not stored. Just like float.
    uint32_t leadingZeros = lzcnt_nonzero(size);
    uint32_t highestSetBit = 31 - leadingZeros;

    uint32_t mantissaStartBit = highestSetBit - MANTISSA_BITS;
    exp = mantissaStartBit + 1;
    mantissa = (size >> mantissaStartBit) & MANTISSA_MASK;

    uint32_t lowBitsMask = (1 << mantissaStartBit) - 1;

    // Round up!
    if ((size & lowBitsMask) != 0)
      mantissa++;
  }

  return (exp << MANTISSA_BITS) +
         mantissa; // + allows mantissa->exp overflow for round up
}

static inline uint32_t uintToFloatRoundDown(uint32_t size) {
  uint32_t exp = 0;
  uint32_t mantissa = 0;

  if (size < MANTISSA_VALUE) {
    // Denorm: 0..(MANTISSA_VALUE-1)
    mantissa = size;
  } else {
    // Normalized: Hidden high bit always 1. Not stored. Just like float.
    uint32_t leadingZeros = lzcnt_nonzero(size);
    uint32_t highestSetBit = 31 - leadingZeros;

    uint32_t mantissaStartBit = highestSetBit - MANTISSA_BITS;
    exp = mantissaStartBit + 1;
    mantissa = (size >> mantissaStartBit) & MANTISSA_MASK;
  }

  return (exp << MANTISSA_BITS) | mantissa;
}

uint32_t floatToUint(uint32_t floatValue) {
  uint32_t exponent = floatValue >> MANTISSA_BITS;
  uint32_t mantissa = floatValue & MANTISSA_MASK;
  if (exponent == 0) {
    // Denorms
    return mantissa;
  } else {
    return (mantissa | MANTISSA_VALUE) << (exponent - 1);
  }
}

static inline void removeNodeFromBin(struct OffsetAlloc *alloc, uint32_t nodeIndex) {
  OffsetAllocNode &node = alloc->m_nodes[nodeIndex];

  if (node.binListPrev != OFFSET_ALLOC_INVALID_INDEX) {
    // Easy case: We have previous node. Just remove this node from the middle
    // of the list.
    alloc->m_nodes[node.binListPrev].binListNext = node.binListNext;
    if (node.binListNext != OFFSET_ALLOC_INVALID_INDEX)
      alloc->m_nodes[node.binListNext].binListPrev = node.binListPrev;
  } else {
    // Hard case: We are the first node in a bin. Find the bin.

    // Round down to bin index to ensure that bin >= alloc
    uint32_t binIndex = uintToFloatRoundDown(node.dataSize);

    uint32_t topBinIndex = binIndex >> OFFSET_ALLOC_TOP_BINS_INDEX_SHIFT;
    uint32_t leafBinIndex = binIndex & OFFSET_ALLOC_LEAF_BINS_INDEX_MASK;

    alloc->m_binIndices[binIndex] = node.binListNext;
    if (node.binListNext != OFFSET_ALLOC_INVALID_INDEX)
      alloc->m_nodes[node.binListNext].binListPrev = OFFSET_ALLOC_INVALID_INDEX;

    // Bin empty?
    if (alloc->m_binIndices[binIndex] == OFFSET_ALLOC_INVALID_INDEX) {
      // Remove a leaf bin mask bit
      alloc->m_usedBins[topBinIndex] &= ~(1 << leafBinIndex);

      // All leaf bins empty?
      if (alloc->m_usedBins[topBinIndex] == 0) {
        // Remove a top bin mask bit
        alloc->m_usedBinsTop &= ~(1 << topBinIndex);
      }
    }
  }

  // Insert the node to freelist
#ifdef DEBUG_VERBOSE
  printf("Putting node %u into freelist[%u] (removeNodeFromBin)\n", nodeIndex,
         m_freeOffset + 1);
#endif
  alloc->m_freeNodes[++alloc->m_freeOffset] = nodeIndex;

  alloc->m_freeStorage -= node.dataSize;
#ifdef DEBUG_VERBOSE
  printf("Free storage: %u (-%u) (removeNodeFromBin)\n", m_freeStorage,
         node.dataSize);
#endif
}

static inline uint32_t insertNodeIntoBin(struct OffsetAlloc *alloc,
                                         uint32_t size, uint32_t dataOffset) {
  // Round down to bin index to ensure that bin >= alloc
  const uint32_t binIndex = uintToFloatRoundDown(size);

  const uint32_t topBinIndex = binIndex >> OFFSET_ALLOC_TOP_BINS_INDEX_SHIFT;
  const uint32_t leafBinIndex = binIndex & OFFSET_ALLOC_LEAF_BINS_INDEX_MASK;

  // Bin was empty before?
  if (alloc->m_binIndices[binIndex] == OFFSET_ALLOC_INVALID_INDEX) {
    // Set bin mask bits
    alloc->m_usedBins[topBinIndex] |= 1 << leafBinIndex;
    alloc->m_usedBinsTop |= 1 << topBinIndex;
  }

  // Take a freelist node and insert on top of the bin linked list (next = old
  // top)
  const uint32_t topNodeIndex = alloc->m_binIndices[binIndex];
  const uint32_t nodeIndex = alloc->m_freeNodes[alloc->m_freeOffset--];

  LOGF(LogLevel::eRAW, "Getting node %u from freelist[%u]\n", nodeIndex,
       alloc->m_freeOffset + 1);
  alloc->m_nodes[nodeIndex] = {
      .dataOffset = dataOffset, .dataSize = size, .binListNext = topNodeIndex};
  if (topNodeIndex != OFFSET_ALLOC_INVALID_INDEX)
    alloc->m_nodes[topNodeIndex].binListPrev = nodeIndex;
  alloc->m_binIndices[binIndex] = nodeIndex;

  alloc->m_freeStorage += size;
  LOGF(LogLevel::eRAW, "Free storage: %u (+%u) (insertNodeIntoBin)\n",
       alloc->m_freeStorage, size);
  return nodeIndex;
}

// Utility functions
static inline uint32_t findLowestSetBitAfter(uint32_t bitMask, uint32_t startBitIndex) {
  uint32_t maskBeforeStartIndex = (1 << startBitIndex) - 1;
  uint32_t maskAfterStartIndex = ~maskBeforeStartIndex;
  uint32_t bitsAfter = bitMask & maskAfterStartIndex;
  if (bitsAfter == 0)
    return OFFSET_ALLOC_NO_SPACE;
  return tzcnt_nonzero(bitsAfter);
}


void offsetAllocFree(struct OffsetAlloc *alloc,
                     struct OffsetAllocation allocation) {
  ASSERT(allocation.metadata != OFFSET_ALLOC_NO_SPACE);
  if (!alloc->m_nodes)
    return;
  uint32_t nodeIndex = allocation.metadata;
  struct OffsetAllocNode &node = alloc->m_nodes[nodeIndex];

  // Double delete check
  ASSERT(node.used == true);

  // Merge with neighbors...
  uint32_t offset = node.dataOffset;
  uint32_t size = node.dataSize;

  if ((node.neighborPrev != OFFSET_ALLOC_INVALID_INDEX) &&
      (alloc->m_nodes[node.neighborPrev].used == false)) {
    // Previous (contiguous) free node: Change offset to previous node offset.
    // Sum sizes
    struct OffsetAllocNode &prevNode = alloc->m_nodes[node.neighborPrev];
    offset = prevNode.dataOffset;
    size += prevNode.dataSize;

    // Remove node from the bin linked list and put it in the freelist
    removeNodeFromBin(alloc, node.neighborPrev);

    ASSERT(prevNode.neighborNext == nodeIndex);
    node.neighborPrev = prevNode.neighborPrev;
  }

  if ((node.neighborNext != OFFSET_ALLOC_INVALID_INDEX) &&
      (alloc->m_nodes[node.neighborNext].used == false)) {
    // Next (contiguous) free node: Offset remains the same. Sum sizes.
    struct OffsetAllocNode &nextNode = alloc->m_nodes[node.neighborNext];
    size += nextNode.dataSize;

    // Remove node from the bin linked list and put it in the freelist
    removeNodeFromBin(alloc, node.neighborNext);

    ASSERT(nextNode.neighborPrev == nodeIndex);
    node.neighborNext = nextNode.neighborNext;
  }

  uint32_t neighborNext = node.neighborNext;
  uint32_t neighborPrev = node.neighborPrev;

  // Insert the removed node to freelist
#ifdef DEBUG_VERBOSE
  printf("Putting node %u into freelist[%u] (free)\n", nodeIndex,
         m_freeOffset + 1);
#endif
  alloc->m_freeNodes[++alloc->m_freeOffset] = nodeIndex;

  // Insert the (combined) free node to bin
  uint32_t combinedNodeIndex = insertNodeIntoBin(alloc, size, offset);

  // Connect neighbors with the new combined node
  if (neighborNext != OFFSET_ALLOC_INVALID_INDEX) {
    alloc->m_nodes[combinedNodeIndex].neighborNext = neighborNext;
    alloc->m_nodes[neighborNext].neighborPrev = combinedNodeIndex;
  }
  if (neighborPrev != OFFSET_ALLOC_INVALID_INDEX) {
    alloc->m_nodes[combinedNodeIndex].neighborPrev = neighborPrev;
    alloc->m_nodes[neighborPrev].neighborNext = combinedNodeIndex;
  }
}

struct OffsetAllocation offsetAllocAlloc(struct OffsetAlloc *alloc,
                                             uint32_t size) {
  if (alloc->m_freeOffset == 0) {
    return {.offset = OFFSET_ALLOC_NO_SPACE, .metadata = OFFSET_ALLOC_NO_SPACE};
  }
  // Round up to bin index to ensure that alloc >= bin
  // Gives us min bin index that fits the size
  uint32_t minBinIndex = uintToFloatRoundUp(size);

  uint32_t minTopBinIndex = minBinIndex >> OFFSET_ALLOC_TOP_BINS_INDEX_SHIFT;
  uint32_t minLeafBinIndex = minBinIndex & OFFSET_ALLOC_LEAF_BINS_INDEX_MASK;

  uint32_t topBinIndex = minTopBinIndex;
  uint32_t leafBinIndex = OFFSET_ALLOC_NO_SPACE;

  // If top bin exists, scan its leaf bin. This can fail (NO_SPACE).
  if (alloc->m_usedBinsTop & (1 << topBinIndex)) {
    leafBinIndex =
        findLowestSetBitAfter(alloc->m_usedBins[topBinIndex], minLeafBinIndex);
  }

  // If we didn't find space in top bin, we search top bin from +1
  if (leafBinIndex == OFFSET_ALLOC_NO_SPACE) {
    topBinIndex =
        findLowestSetBitAfter(alloc->m_usedBinsTop, minTopBinIndex + 1);

    // Out of space?
    if (topBinIndex == OFFSET_ALLOC_NO_SPACE) {
      return {.offset = OFFSET_ALLOC_NO_SPACE,
              .metadata = OFFSET_ALLOC_NO_SPACE};
    }

    // All leaf bins here fit the alloc, since the top bin was rounded up. Start
    // leaf search from bit 0. NOTE: This search can't fail since at least one
    // leaf bit was set because the top bit was set.
    leafBinIndex = tzcnt_nonzero(alloc->m_usedBins[topBinIndex]);
  }

  uint32_t binIndex =
      (topBinIndex << OFFSET_ALLOC_TOP_BINS_INDEX_SHIFT) | leafBinIndex;

  // Pop the top node of the bin. Bin top = node.next.
  uint32_t nodeIndex = alloc->m_binIndices[binIndex];
  OffsetAllocNode &node = alloc->m_nodes[nodeIndex];
  uint32_t nodeTotalSize = node.dataSize;
  node.dataSize = size;
  node.used = true;
  alloc->m_binIndices[binIndex] = node.binListNext;
  if (node.binListNext != OFFSET_ALLOC_INVALID_INDEX)
    alloc->m_nodes[node.binListNext].binListPrev = OFFSET_ALLOC_INVALID_INDEX;
  alloc->m_freeStorage -= nodeTotalSize;
#ifdef DEBUG_VERBOSE
  printf("Free storage: %u (-%u) (allocate)\n", m_freeStorage, nodeTotalSize);
#endif

  // Bin empty?
  if (alloc->m_binIndices[binIndex] == OFFSET_ALLOC_INVALID_INDEX) {
    // Remove a leaf bin mask bit
    alloc->m_usedBins[topBinIndex] &= ~(1 << leafBinIndex);

    // All leaf bins empty?
    if (alloc->m_usedBins[topBinIndex] == 0) {
      // Remove a top bin mask bit
      alloc->m_usedBinsTop &= ~(1 << topBinIndex);
    }
  }

  // Push back reminder N elements to a lower bin
  uint32_t reminderSize = nodeTotalSize - size;
  if (reminderSize > 0) {
    uint32_t newNodeIndex =
        insertNodeIntoBin(alloc, reminderSize, node.dataOffset + size);

    // Link nodes next to each other so that we can merge them later if both are
    // free And update the old next neighbor to point to the new node (in
    // middle)
    if (node.neighborNext != OFFSET_ALLOC_INVALID_INDEX)
      alloc->m_nodes[node.neighborNext].neighborPrev = newNodeIndex;
    alloc->m_nodes[newNodeIndex].neighborPrev = nodeIndex;
    alloc->m_nodes[newNodeIndex].neighborNext = node.neighborNext;
    node.neighborNext = newNodeIndex;
  }

  return {.offset = node.dataOffset, .metadata = nodeIndex};
}

struct OffsetAllocReport offsetAllocStorageReport(struct OffsetAlloc *alloc) {
  uint32_t largestFreeRegion = 0;
  uint32_t freeStorage = 0;

  // Out of allocations? -> Zero free space
  if (alloc->m_freeOffset > 0) {
    freeStorage = alloc->m_freeStorage;
    if (alloc->m_usedBinsTop) {
      uint32_t topBinIndex = 31 - lzcnt_nonzero(alloc->m_usedBinsTop);
      uint32_t leafBinIndex =
          31 - lzcnt_nonzero(alloc->m_usedBins[topBinIndex]);
      largestFreeRegion = floatToUint(
          (topBinIndex << OFFSET_ALLOC_TOP_BINS_INDEX_SHIFT) | leafBinIndex);
      ASSERT(freeStorage >= largestFreeRegion);
    }
  }

  return {.totalFreeSpace = freeStorage,
          .largestFreeRegion = largestFreeRegion};
}

struct OffsetAllocReportFull offsetAllocStorageReportFull(struct OffsetAlloc *alloc) {
  OffsetAllocReportFull report;
  for (uint32_t i = 0; i < OFFSET_ALLOC_NUM_LEAF_BINS; i++) {
    uint32_t count = 0;
    uint32_t nodeIndex = alloc->m_binIndices[i];
    while (nodeIndex != OFFSET_ALLOC_INVALID_INDEX) {
      nodeIndex = alloc->m_nodes[nodeIndex].binListNext;
      count++;
    }
    report.freeRegions[i] = {.size = floatToUint(i), .count = count};
  }
  return report;
}

void resetOffsetAlloc(struct OffsetAlloc *alloc) {

  alloc->m_freeStorage = 0;
  alloc->m_usedBinsTop = 0;
  alloc->m_freeOffset = alloc->m_maxAllocs - 1;
  memset(alloc->m_usedBins, 0, sizeof(alloc->m_usedBins));
  memset(alloc->m_binIndices, 0, sizeof(alloc->m_binIndices));

  if (alloc->m_nodes)
    tf_free(alloc->m_nodes);
  if (alloc->m_freeNodes)
    tf_free(alloc->m_freeNodes);

  alloc->m_nodes = (struct OffsetAllocNode *)tf_calloc(
      alloc->m_maxAllocs, sizeof(struct OffsetAllocNode));
  alloc->m_freeNodes =
      (uint32_t *)tf_calloc(alloc->m_maxAllocs, sizeof(uint32_t));

  for (uint32_t i = 0; i < alloc->m_maxAllocs; i++) {
    alloc->m_freeNodes[i] = alloc->m_maxAllocs - i - 1;

    alloc->m_nodes->binListPrev = OFFSET_ALLOC_INVALID_INDEX;
    alloc->m_nodes->binListNext = OFFSET_ALLOC_INVALID_INDEX;
    alloc->m_nodes->neighborPrev = OFFSET_ALLOC_INVALID_INDEX;
    alloc->m_nodes->neighborNext = OFFSET_ALLOC_INVALID_INDEX;
  }

  // Start state: Whole storage as one big node
  // Algorithm will split remainders and push them back as smaller nodes
  insertNodeIntoBin(alloc, alloc->m_size, 0);
}
