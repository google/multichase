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

void print_page_size(size_t page_size, bool use_thp) {
  FILE *f;
  size_t read;
  /* Big enough to fit UINT64_MAX + '\n' + '\0'. */
  char buf[22];

  if (!use_thp) {
    printf("page_size = %zu bytes\n", page_size);
    return;
  }

  f = fopen("/sys/kernel/mm/transparent_hugepage/hpage_pmd_size", "r");
  if (!f) goto err;

  read = fread(buf, 1, sizeof(buf) - 1, f);
  if (!feof(f) || (ferror(f) && !feof(f))) goto err;

  if (fclose(f)) goto err;

  if (buf[read - 1] == '\n') --read;
  buf[read] = '\0';

  printf("page_size = %s bytes (THP)\n", buf);
  return;

err:
  perror(
      "page_size = <failed to read "
      "/sys/kernel/mm/transparent_hugepage/hpage_pmd_size>");
}

static inline int mbind(void *addr, unsigned long len, int mode,
                        unsigned long *nodemask, unsigned long maxnode,
                        unsigned flags) {
  return syscall(__NR_mbind, addr, len, mode, nodemask, maxnode, flags);
}

static void arena_weighted_mbind(size_t page_size, void *arena,
                                 size_t arena_size, uint16_t *weights,
                                 size_t nr_weights) {
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

  uint64_t mask = 0;
  char *q = (char *)arena + arena_size;
  rng_init(1);
  for (char *p = arena; p < q; p += page_size) {
    uint32_t r = rng_int(1 << 31) % weight_sum;
    unsigned int node;
    for (node = 0; node < nr_weights; node++) {
      if (weights_cumsum[node] >= r) {
        break;
      }
    }
    mask = 1 << node;
    if (mbind(p, page_size, MPOL_BIND, &mask, nr_weights, MPOL_MF_STRICT)) {
      perror("mbind");
      exit(1);
    }
    *p = 0;
  }
  free(weights_cumsum);
}

static int get_page_size_flags(size_t page_size) {
  int lg = 0;

  if (!page_size || (page_size & (page_size - 1))) {
    fprintf(stderr, "page size must be a power of 2: %zu\n", page_size);
    exit(1);
  }

  if (!page_size_is_huge(page_size)) {
    return 0;
  }

  /*
   * We need not just MAP_HUGETLB, but also a flag specifying the page size.
   * mmap(2) says that these flags are defined as:
   * log2(page size) << MAP_HUGE_SHIFT.
   */
  while (page_size >>= 1) {
    ++lg;
  }
  return MAP_HUGETLB | (lg << MAP_HUGE_SHIFT);
}

/*
 * Reads a "state" file from sysfs at the given path, and returns the current
 * state. The caller must free() the returned pointer when finished with it.
 *
 * The file must be formatted like this:
 *
 * state1 state2 [state3] state4
 *
 * The state surrounded by []s is the currently active one. It is returned
 * as-is, including the surrounding []s.
 */
static char *read_sysfs_state_file(char const *path) {
  FILE *f = fopen(path, "r");
  char *token = NULL;
  int ret;

  if (f == NULL) {
    perror("open sysfs state file");
    exit(1);
  }

  while ((ret = fscanf(f, "%ms", &token)) == 1) {
    if (token[0] == '[') break;

    free(token);
    token = NULL;
  }

  if (ferror(f) && !feof(f)) {
    perror("read sysfs state file");
    exit(1);
  }

  if (fclose(f)) {
    perror("close sysfs state file");
    exit(1);
  }

  return token;
}

static void write_sysfs_file(char const *path, char const *value) {
  FILE *f = fopen(path, "w");

  if (f == NULL) {
    perror("open sysfs file for write");
    exit(1);
  }

  if (fprintf(f, "%s\n", value) < 0) {
    perror("write value to sysfs file");
    exit(1);
  }

  if (fclose(f)) {
    perror("close sysfs file");
    exit(1);
  }
}

/*
 * In order for MADV_HUGEPAGE to work, THP configuration must be in one of
 * several acceptable states. Check if the existing system configuration is
 * acceptable, and if not, try to change the configuration.
 */
static void check_thp_state(void) {
  char *enabled =
      read_sysfs_state_file("/sys/kernel/mm/transparent_hugepage/enabled");
  char *defrag =
      read_sysfs_state_file("/sys/kernel/mm/transparent_hugepage/defrag");

  if (strcmp(enabled, "[always]") && strcmp(enabled, "[madvise]")) {
    write_sysfs_file("/sys/kernel/mm/transparent_hugepage/enabled", "madvise");
  }

  if (strcmp(defrag, "[always]") && strcmp(defrag, "[defer+madvise]") &&
      strcmp(defrag, "[madvise]")) {
    write_sysfs_file("/sys/kernel/mm/transparent_hugepage/defrag", "madvise");
  }

  free(enabled);
  free(defrag);
}

void *alloc_arena_mmap(size_t page_size, bool use_thp, size_t arena_size) {
  void *arena;
  size_t pagemask = page_size - 1;
  int flags = MAP_PRIVATE | MAP_ANONYMOUS | get_page_size_flags(page_size);

  arena_size = (arena_size + pagemask) & ~pagemask;
  arena = mmap(0, arena_size, PROT_READ | PROT_WRITE, flags, -1, 0);
  if (arena == MAP_FAILED) {
    perror("mmap");
    exit(1);
  }

  if (use_thp) check_thp_state();

  /* Explicitly disable THP for small pages. */
  if (!page_size_is_huge(page_size)) {
    if (madvise(arena, arena_size, use_thp ? MADV_HUGEPAGE : MADV_NOHUGEPAGE)) {
      perror("madvise");
    }
  } else if (use_thp) {
    fprintf(stderr,
            "Can't use transparent hugepages with a non-native page size.\n");
    exit(1);
  }

  if (is_weighted_mbind) {
    arena_weighted_mbind(page_size, arena, arena_size, mbind_weights,
                         MAX_MEM_NODES);
  }
  return arena;
}
