#pragma once

#include "assert.h"
#include "math.h"
#include "type.h"

#if __has_builtin(__builtin_bzero)
#define bzero(address, size) __builtin_bzero(address, size)
#else
#error bzero must be supported by compiler
#endif

#if __has_builtin(__builtin_alloca)
#define alloca(size) __builtin_alloca(size)
#else
#error alloca must be supported by compiler
#endif

#if __has_builtin(__builtin_memcpy)
#define memcpy(dest, src, n) __builtin_memcpy(dest, src, n)
#else
#error memcpy must be supported by compiler
#endif

struct memory_arena {
  void *block;
  u64 used;
  u64 total;
};

struct memory_chunk {
  void *block;
  u64 size;
  u64 max;
};

struct memory_temp {
  struct memory_arena *arena;
  u64 startedAt;
};

static struct memory_arena MemoryArenaSub(struct memory_arena *master,
                                          u64 size) {
  debug_assert(master->used + size <= master->total);

  struct memory_arena sub = {
      .total = size,
      .block = master->block + master->used,
  };

  master->used += size;
  return sub;
}

static void *MemoryArenaPushUnaligned(struct memory_arena *mem, u64 size) {
  debug_assert(mem->used + size <= mem->total);
  void *result = mem->block + mem->used;
  mem->used += size;
  return result;
}

static void *MemoryArenaPush(struct memory_arena *mem, u64 size,
                             u64 alignment) {
  debug_assert(IsPowerOfTwo(alignment));

  void *block = mem->block + mem->used;

  u64 alignmentMask = alignment - 1;
  u64 alignmentResult = ((u64)block & alignmentMask);
  if (alignmentResult != 0) {
    // if it is not aligned
    u64 alignmentOffset = alignment - alignmentResult;
    size += alignmentOffset;
    block += alignmentOffset;
  }

  debug_assert(mem->used + size <= mem->total);
  mem->used += size;

  return block;
}

static struct memory_chunk *MemoryArenaPushChunk(struct memory_arena *mem,
                                                 u64 size, u64 max) {
  struct memory_chunk *chunk =
      MemoryArenaPush(mem, sizeof(*chunk) + max * sizeof(u8) + max * size, 4);
  chunk->block = (u8 *)chunk + sizeof(*chunk);
  chunk->size = size;
  chunk->max = max;
  for (u64 index = 0; index < chunk->max; index++) {
    u8 *flag = (u8 *)chunk->block + (sizeof(u8) * index);
    *flag = 0;
  }
  return chunk;
}

static inline b8 MemoryChunkIsDataAvailableAt(struct memory_chunk *chunk,
                                              u64 index) {
  u8 *flags = (u8 *)chunk->block;
  return *(flags + index);
}

static inline void *MemoryChunkGetDataAt(struct memory_chunk *chunk,
                                         u64 index) {
  void *dataBlock = (u8 *)chunk->block + chunk->max;
  void *result = dataBlock + index * chunk->size;
  return result;
}

static void *MemoryChunkPush(struct memory_chunk *chunk) {
  void *result = 0;
  void *dataBlock = chunk->block + sizeof(u8) * chunk->max;
  for (u64 index = 0; index < chunk->max; index++) {
    u8 *flag = chunk->block + sizeof(u8) * index;
    if (*flag == 0) {
      result = dataBlock + index * chunk->size;
      *flag = 1;
      return result;
    }
  }

  return result;
}

static void MemoryChunkPop(struct memory_chunk *chunk, void *block) {
  void *dataBlock = chunk->block + sizeof(u8) * chunk->max;
  debug_assert(
      (block >= dataBlock && block <= dataBlock + (chunk->size * chunk->max)) &&
      "this block is not belong to this chunk");
  u64 index = ((u64)block - (u64)dataBlock) / chunk->size;
  u8 *flag = chunk->block + sizeof(u8) * index;
  *flag = 0;
}

static struct memory_temp MemoryTempBegin(struct memory_arena *arena) {
  return (struct memory_temp){
      .arena = arena,
      .startedAt = arena->used,
  };
}

static void MemoryTempEnd(struct memory_temp *tempMemory) {
  struct memory_arena *arena = tempMemory->arena;
  arena->used = tempMemory->startedAt;
}

#include "text.h"
static struct string MemoryArenaPushString(struct memory_arena *arena,
                                           u64 size) {
  return (struct string){
      .value = MemoryArenaPush(arena, size, 4),
      .length = size,
  };
}
