#include "memory.h"

// TODO: Show error pretty error message when a test fails
enum memory_test_error {
  MEMORY_TEST_ERROR_NONE = 0,
  MEMORY_TEST_ERROR_MEM_PUSH_EXPECTED_VALID_ADDRESS_1,
  MEMORY_TEST_ERROR_MEM_PUSH_EXPECTED_VALID_ADDRESS_2,
  MEMORY_TEST_ERROR_MEM_CHUNK_PUSH_EXPECTED_VALID_ADDRESS_1,
  MEMORY_TEST_ERROR_MEM_CHUNK_PUSH_EXPECTED_VALID_ADDRESS_2,
  MEMORY_TEST_ERROR_MEM_CHUNK_PUSH_EXPECTED_VALID_ADDRESS_3,
  MEMORY_TEST_ERROR_MEM_CHUNK_PUSH_EXPECTED_NULL,
  MEMORY_TEST_ERROR_MEM_CHUNK_POP_EXPECTED_SAME_ADDRESS,

  // src: https://mesonbuild.com/Unit-tests.html#skipped-tests-and-hard-errors
  // For the default exitcode testing protocol, the GNU standard approach in
  // this case is to exit the program with error code 77. Meson will detect this
  // and report these tests as skipped rather than failed. This behavior was
  // added in version 0.37.0.
  MESON_TEST_SKIP = 77,
  // In addition, sometimes a test fails set up so that it should fail even if
  // it is marked as an expected failure. The GNU standard approach in this case
  // is to exit the program with error code 99. Again, Meson will detect this
  // and report these tests as ERROR, ignoring the setting of should_fail. This
  // behavior was added in version 0.50.0.
  MESON_TEST_FAILED_TO_SET_UP = 99,
};

int main(void) {
  enum memory_test_error errorCode = MEMORY_TEST_ERROR_NONE;
  struct memory_arena memory;
  struct memory_temp tempMemory;

  {
    u64 KILOBYTES = 1 << 10;
    u64 total = 1 * KILOBYTES;
    memory = (struct memory_arena){.block = alloca(total), .total = total};
    if (memory.block == 0) {
      errorCode = MESON_TEST_FAILED_TO_SET_UP;
      goto end;
    }
    bzero(memory.block, memory.total);
  }

  // MemPush(struct memory_block *mem, u64 size)
  tempMemory = MemoryTempBegin(&memory);
  {
    void *expected;
    void *value;

    expected = memory.block;
    value = MemoryArenaPush(&memory, 16, 4);
    if (value != expected) {
      errorCode = MEMORY_TEST_ERROR_MEM_PUSH_EXPECTED_VALID_ADDRESS_1;
      goto end;
    }

    expected = memory.block + 16;
    value = MemoryArenaPush(&memory, 16, 4);
    if (value != expected) {
      errorCode = MEMORY_TEST_ERROR_MEM_PUSH_EXPECTED_VALID_ADDRESS_2;
      goto end;
    }
  }
  MemoryTempEnd(&tempMemory);

  // MemoryChunkPush(struct memory_chunk *chunk)
  tempMemory = MemoryTempBegin(&memory);
  {
    struct memory_chunk *chunk = MemoryArenaPushChunk(&memory, 16, 3);
    void *expected;
    void *value;
    u64 index;
    u8 flag;

    /*
     * | memory_chunk                               |
     * |--------------------------------------------|
     * | flags -> max*sizeof(u8) | data -> max*size |
     */
    void *flagStartAddress = (u8 *)chunk->block;
    void *dataStartAddress = (u8 *)chunk->block + (sizeof(u8) * chunk->max);

    index = 0;
    expected = dataStartAddress + (16 * index);
    value = MemoryChunkPush(chunk);
    flag = *((u8 *)flagStartAddress + index);
    if (value != expected || flag != 1) {
      errorCode = MEMORY_TEST_ERROR_MEM_CHUNK_PUSH_EXPECTED_VALID_ADDRESS_1;
      goto end;
    }

    index = 1;
    expected = dataStartAddress + (16 * index);
    value = MemoryChunkPush(chunk);
    flag = *((u8 *)flagStartAddress + index);
    if (value != expected || flag != 1) {
      errorCode = MEMORY_TEST_ERROR_MEM_CHUNK_PUSH_EXPECTED_VALID_ADDRESS_2;
      goto end;
    }

    index = 2;
    expected = dataStartAddress + (16 * index);
    value = MemoryChunkPush(chunk);
    flag = *((u8 *)flagStartAddress + index);
    if (value != expected || flag != 1) {
      errorCode = MEMORY_TEST_ERROR_MEM_CHUNK_PUSH_EXPECTED_VALID_ADDRESS_3;
      goto end;
    }

    expected = 0;
    value = MemoryChunkPush(chunk);
    if (value != expected) {
      errorCode = MEMORY_TEST_ERROR_MEM_CHUNK_PUSH_EXPECTED_NULL;
      goto end;
    }
  }
  MemoryTempEnd(&tempMemory);

  // MemoryChunkPop(struct memory_chunk *chunk, void *block)
  tempMemory = MemoryTempBegin(&memory);
  {
    struct memory_chunk *chunk = MemoryArenaPushChunk(&memory, 16, 3);
    void *expected;
    void *value;

    expected = MemoryChunkPush(chunk);
    MemoryChunkPop(chunk, expected);
    value = MemoryChunkPush(chunk);
    if (value != expected) {
      errorCode = MEMORY_TEST_ERROR_MEM_CHUNK_POP_EXPECTED_SAME_ADDRESS;
      goto end;
    }
  }
  MemoryTempEnd(&tempMemory);

end:
  return (int)errorCode;
}
