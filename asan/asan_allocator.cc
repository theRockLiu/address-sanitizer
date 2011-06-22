/* Copyright 2011 Google Inc.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

// This file is a part of AddressSanitizer, an address sanity checker.
// *************
//  NOTE: this file is not used by the rtl yet
// *************

#include "asan_allocator.h"
#include "asan_int.h"
#include "asan_mapping.h"
#include "asan_rtl.h"
#include "asan_stats.h"

#include <sys/mman.h>
#include <stdint.h>
#include <string.h>
#include <algorithm>

void *(*__asan_real_malloc)(size_t);
void (*__asan_real_free)(void *ptr);

namespace {

static const size_t kRedzone      = kMinRedzone * 2;
static const size_t kMinAllocSize = kRedzone * 2;
static const size_t kMinMmapSize  = kPageSize * 128;


static inline bool IsAligned(uintptr_t a, uintptr_t alignment) {
  return (a & (alignment - 1)) == 0;
}

static inline bool IsWordAligned(uintptr_t a) {
  return IsAligned(a, kWordSize);
}

static inline bool IsPowerOfTwo(size_t x) {
  return (x & (x - 1)) == 0;
}

static inline size_t Log2(size_t x) {
  CHECK(IsPowerOfTwo(x));
  return __builtin_ctzl(x);
}

static inline size_t RoundUptoRedzone(size_t size) {
  return ((size + kRedzone - 1) / kRedzone) * kRedzone;
}

static inline size_t RoundUptoPowerOfTwo(size_t size) {
  CHECK(size);
  if (IsPowerOfTwo(size)) return size;
  size_t up = __WORDSIZE - __builtin_clzl(size);
  CHECK(size < (1UL << up));
  CHECK(size > (1UL << (up - 1)));
  return 1UL << up;
}

static void PoisonShadow(uintptr_t mem, size_t size, uint8_t poison) {
  CHECK(IsAligned(mem,        1 << kShadowShift));
  CHECK(IsAligned(mem + size, 1 << kShadowShift));
  uintptr_t shadow_beg = MemToShadow(mem);
  uintptr_t shadow_end = MemToShadow(mem + size);
  memset((void*)shadow_beg, poison, shadow_end - shadow_beg);
}

static uint8_t *MmapNewPages(size_t size) {
  CHECK((size % kPageSize) == 0);
  uint8_t *res = (uint8_t*)mmap(0, size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
  if (res == (uint8_t*)-1) {
    Printf("failed to mmap %ld bytes\n", size);
    abort();
  }
  PoisonShadow((uintptr_t)res, size, -1);
  return res;
}

// Every chunk of memory allocated by this allocator can be in one of 3 states:
// CHUNK_AVAILABLE: the chunk is in the free list and ready to be allocated.
// CHUNK_ALLOCATED: the chunk is allocated and not yet freed.
// CHUNK_QUARANTINE: the chunk was freed and put into quarantine zone.
//
// The pseudo state CHUNK_MEMALIGN is used to mark that the address is not 
// the beginning of a Chunk (in which case the next work contains the address
// of the Chunk).
//
// The magic numbers for the enum values are taken randomly.
enum {
  CHUNK_AVAILABLE  = 0x573B5CE5,
  CHUNK_ALLOCATED  = 0x32041A36,
  CHUNK_QUARANTINE = 0x1978BAE3,
  CHUNK_MEMALIGN   = 0xDC68ECD8,
};

struct Chunk {
  uintptr_t    chunk_state;     // Should be the first field.
  size_t       allocated_size;  // Must be power of two
  size_t       used_size;
  Chunk       *next;
  Chunk       *prev;
};

class MallocInfo {
 public:
  Chunk *AllocateChunk(size_t size) {
    CHECK(IsPowerOfTwo(size));
    size_t idx = Log2(size);
    if (!chunks[idx]) {
      GetNewChunks(size);
    }
    Chunk *m = chunks[idx];
    CHECK(m);
    chunks[idx] = m->next;
    m->next = m->prev = 0;
    CHECK(m->chunk_state == CHUNK_AVAILABLE);
    m->chunk_state = CHUNK_ALLOCATED;

    if (malloced_items_) {
      malloced_items_->prev = m;
    }
    m->next = malloced_items_;
    malloced_items_ = m;
    return m;
  }

  void TakeChunkBack(Chunk *m) {
    CHECK(m);
    CHECK(m->chunk_state == CHUNK_ALLOCATED);
    CHECK(IsPowerOfTwo(m->allocated_size));
    CHECK(__asan_flag_quarantine_size > 0);

    // remove from malloced list.
    {
      if (m == malloced_items_) {
        malloced_items_ = m->next;
        if (malloced_items_)
          malloced_items_->prev = 0;
      } else {
        Chunk *prev = m->prev;
        Chunk *next = m->next;
        if (prev) prev->next = next;
        if (next) next->prev = prev;
      }
    }

    if (!quarantine_) {
      m->next = m->prev = m;
    } else {
      Chunk *prev = quarantine_->prev;
      Chunk *next = quarantine_;
      m->next = next;
      m->prev = prev;
      prev->next = m;
      next->prev = m;
    }
    quarantine_ = m;
    quarantine_size_ += m->allocated_size;
    m->chunk_state = CHUNK_QUARANTINE;
    while (quarantine_size_ &&
           (quarantine_size_ > __asan_flag_quarantine_size)) {
      pop();
    }
  }

  void Deallocate(Chunk *m) {
    CHECK(m);
    CHECK(m->chunk_state == CHUNK_ALLOCATED);
    TakeChunkBack(m);
  }

  void GetNewChunks(size_t size) {
    size_t idx = Log2(size);
    CHECK(chunks[idx] == NULL);
    CHECK(IsPowerOfTwo(size));
    CHECK(IsPowerOfTwo(kMinMmapSize));
    size_t mmap_size = std::max(size, kMinMmapSize);
    CHECK(IsPowerOfTwo(mmap_size));
    uint8_t *mem = MmapNewPages(mmap_size);
    for (size_t i = 0; i < mmap_size / size; i++) {
      Chunk *m = (Chunk*)(mem + i * size);
      m->chunk_state = CHUNK_AVAILABLE;
      m->allocated_size = size;
      m->next = chunks[idx];
      chunks[idx] = m;
    }
  }

 private:
  void pop() {
    CHECK(quarantine_);
    CHECK(quarantine_size_ > 0);
    Chunk *m = quarantine_->prev;
    CHECK(m);
    // Printf("pop  : %p quarantine_size_ = %ld; size = %ld\n", m, quarantine_size_, m->size);
    Chunk *next = m->next;
    Chunk *prev = m->prev;
    CHECK(next && prev);
    if (next == m) {
      quarantine_ = NULL;
    } else {
      next->prev = prev;
      prev->next = next;
    }
    CHECK(quarantine_size_ >= m->allocated_size);
    quarantine_size_ -= m->allocated_size;
    // if (F_v >= 2) Printf("MallocInfo::pop %p\n", m);

    CHECK(m->chunk_state == CHUNK_QUARANTINE);
    m->chunk_state = CHUNK_AVAILABLE;
    size_t idx = Log2(m->allocated_size);
    m->next = chunks[idx];
    chunks[idx] = m;
  }

  Chunk *chunks[__WORDSIZE];
  Chunk *quarantine_;
  size_t quarantine_size_;
  Chunk *malloced_items_;
};

static MallocInfo malloc_info;

static uint8_t *Allocate(size_t alignment, size_t size, AsanStackTrace *stack) {
  // Printf("Allocate align: %ld size: %ld\n", alignment, size);
  if (size == 0) return NULL;
  CHECK(IsPowerOfTwo(alignment));
  size_t rounded_size = RoundUptoRedzone(size);
  size_t needed_size = rounded_size + kRedzone;
  if (alignment > kRedzone) {
    needed_size += alignment;
  }
  CHECK((needed_size % kRedzone) == 0);
  size_t size_to_allocate = RoundUptoPowerOfTwo(needed_size);
  CHECK(size_to_allocate >= kMinAllocSize);
  CHECK((size_to_allocate % kRedzone) == 0);

  Chunk *m = malloc_info.AllocateChunk(size_to_allocate);
  CHECK(m);
  CHECK(m->allocated_size == size_to_allocate);
  CHECK(m->chunk_state == CHUNK_ALLOCATED);
  m->used_size = size;
  uintptr_t addr = (uintptr_t)m + kRedzone;

  if (alignment > kRedzone && (addr & (alignment - 1))) {
    size_t alignment_log = Log2(alignment);
    // Printf("xx1 "PP"\n", addr);
    addr = ((addr + alignment - 1) >> alignment_log) << alignment_log;
    CHECK((addr & (alignment - 1)) == 0);
    uintptr_t *p = (uintptr_t*)(addr - kRedzone);
    p[0] = CHUNK_MEMALIGN;
    p[1] = (uintptr_t)m;
  }
  PoisonShadow(addr, rounded_size, 0);
  // Printf("ret "PP"\n", addr);
  return (uint8_t*)addr;
}
__attribute__((noinline))
static void asan_clear_mem(uintptr_t *mem, size_t n_words) {
  CHECK(IsWordAligned((uintptr_t)mem));
  for (size_t i = 0; i < n_words; i++)
    mem[i] = 0;
}

__attribute__((noinline))
static void asan_copy_mem(uintptr_t *dst, uintptr_t *src, size_t n_words) {
  CHECK(IsWordAligned((uintptr_t)dst));
  CHECK(IsWordAligned((uintptr_t)src));
  for (size_t i = 0; i < n_words; i++) {
    dst[i] = src[i];
  }
}

static Chunk *PtrToChunk(uint8_t *ptr) {
  Chunk *m = (Chunk*)(ptr - kRedzone);
  if (m->chunk_state == CHUNK_MEMALIGN) {
    m = (Chunk*)((uintptr_t*)m)[1];
  }
  return m;
}

static void Deallocate(uint8_t *ptr, AsanStackTrace *stack) {
  if (!ptr) return;
  Chunk *m = PtrToChunk(ptr);
  CHECK(m->chunk_state == CHUNK_ALLOCATED);
  size_t rounded_size = RoundUptoRedzone(m->used_size);
  PoisonShadow((uintptr_t)ptr, rounded_size, -1);
  malloc_info.Deallocate(m);
}

static uint8_t *Reallocate(uint8_t *ptr, size_t size, AsanStackTrace *stack) {
  if (!ptr) {
    return Allocate(0, size, stack);
  }
  if (size == 0) {
    return NULL;
  }
  Chunk *m = PtrToChunk(ptr);
  CHECK(m->chunk_state == CHUNK_ALLOCATED);
  size_t old_size = m->used_size;
  size_t memcpy_size = std::min(size, old_size);
  uint8_t *new_ptr = Allocate(0, size, stack);
  asan_copy_mem((uintptr_t*)new_ptr, (uintptr_t*)ptr,
                (memcpy_size + kWordSize - 1) / kWordSize);
  Deallocate(ptr, stack);
  __asan_stats.reallocs++;
  __asan_stats.realloced += memcpy_size;
  return new_ptr;
}

}  // namespace

void *__asan_memalign(size_t alignment, size_t size, AsanStackTrace *stack) {
  return (void*)Allocate(alignment, size, stack);
}

void __asan_free(void *ptr, AsanStackTrace *stack) {
  Deallocate((uint8_t*)ptr, stack);
}

void *__asan_malloc(size_t size, AsanStackTrace *stack) {
  return (void*)Allocate(0, size, stack);
}

void *__asan_calloc(size_t nmemb, size_t size, AsanStackTrace *stack) {
  uint8_t *res = Allocate(0, nmemb * size, stack);
  asan_clear_mem((uintptr_t*)res, (nmemb * size + kWordSize - 1) / kWordSize);
  return (void*)res;
}
void *__asan_realloc(void *p, size_t size, AsanStackTrace *stack) {
  return Reallocate((uint8_t*)p, size, stack);
}

void *__asan_valloc(size_t size, AsanStackTrace *stack) {
  return Allocate(kPageSize, size, stack);
}

int __asan_posix_memalign(void **memptr, size_t alignment, size_t size,
                          AsanStackTrace *stack) {
  *memptr = Allocate(alignment, size, stack);
  CHECK(((uintptr_t)*memptr % alignment) == 0);
  return 0;
}

size_t __asan_mz_size(const void *ptr) {
  CHECK(0);
  return 0;
}

void __asan_describe_heap_address(uintptr_t addr, uintptr_t access_size) {
  CHECK(0);
}
