/* Copyright 2015 Google Inc. All Rights Reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "arena.h"

#include <linux/mempolicy.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "permutation.h"

extern int verbosity;
extern int is_weighted_mbind;
extern uint16_t mbind_weights[MAX_MEM_NODES];

size_t get_native_page_size(void) {
  long sz;

  sz = sysconf(_SC_PAGESIZE);
  if (sz < 0) {
    perror("failed to get native page size");
    exit(1);
  }

  return (size_t)sz;
}

bool page_size_is_huge(size_t page_size) {
  return page_size > get_native_page_size();
}

static inline int mbind(void *addr, unsigned long len, int mode,
                        unsigned long *nodemask, unsigned long maxnode,
                        unsigned flags) {
  return syscall(__NR_mbind, addr, len, mode, nodemask, maxnode, flags);
}

static void arena_weighted_mbind(void *arena, size_t arena_size,
                                 uint16_t *weights, size_t nr_weights) {
  /* compute cumulative sum for weights
   * cumulative sum starts at -1
   * the method for determining a hit on a weight i is when the generated
   * random number (modulo sum of weights) <= weights_cumsum[i]
   */
  int64_t *weights_cumsum = malloc(nr_weights * sizeof(int64_t));
  if (!weights_cumsum) {
    fprintf(stderr, "Couldn't allocate memory for weights.\n");
    exit(1);
  }
  weights_cumsum[0] = weights[0] - 1;
  for (unsigned int i = 1; i < nr_weights; i++) {
    weights_cumsum[i] = weights_cumsum[i - 1] + weights[i];
  }
  const int32_t weight_sum = weights_cumsum[nr_weights - 1] + 1;
  const int pagesize = getpagesize();

  uint64_t mask = 0;
  char *q = (char *)arena + arena_size;
  rng_init(1);
  for (char *p = arena; p < q; p += pagesize) {
    uint32_t r = rng_int(1 << 31) % weight_sum;
    unsigned int node;
    for (node = 0; node < nr_weights; node++) {
      if (weights_cumsum[node] >= r) {
        break;
      }
    }
    mask = 1 << node;
    if (mbind(p, pagesize, MPOL_BIND, &mask, nr_weights, MPOL_MF_STRICT)) {
      perror("mbind");
      exit(1);
    }
    *p = 0;
  }
  free(weights_cumsum);
}

static int get_page_size_flags(size_t page_size) {
  if (!page_size_is_huge(page_size)) {
    return 0;
  }

  fprintf(stderr, "unsupported page size: %zu\n", page_size);
  exit(1);
}

void *alloc_arena_mmap(size_t page_size, size_t arena_size) {
  void *arena;
  size_t pagemask = page_size - 1;
  int flags = MAP_PRIVATE | MAP_ANONYMOUS | get_page_size_flags(page_size);

  arena_size = (arena_size + pagemask) & ~pagemask;
  arena = mmap(0, arena_size, PROT_READ | PROT_WRITE, flags, -1, 0);
  if (arena == MAP_FAILED) {
    perror("mmap");
    exit(1);
  }

  /* Explicitly disable THP for small pages. */
  if (!page_size_is_huge(page_size)) {
    if (madvise(arena, arena_size, MADV_NOHUGEPAGE)) {
      perror("madvise");
    }
  }

  if (is_weighted_mbind) {
    arena_weighted_mbind(arena, arena_size, mbind_weights, MAX_MEM_NODES);
  }
  return arena;
}
