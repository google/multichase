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
#include "permutation.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

// some asserts are more expensive than we want in general use, but there are a
// few i want active even in general use.
#if 1
#define dassert(x) \
  do {             \
  } while (0)
#else
#define dassert(x) assert(x)
#endif

// XXX: declare this somewhere
extern int verbosity;

__thread char *rng_buf;  // buf size (32 for now) determines the "randomness"
__thread struct random_data *rand_state;  // per_thread state for random_r

//============================================================================
// a random permutation generator.  i think this algorithm is from Knuth.

void gen_random_permutation(perm_t *perm, size_t nr, size_t base) {
  size_t i;

  for (i = 0; i < nr; ++i) {
    size_t t = rng_int(i);
    perm[i] = perm[t];
    perm[t] = base + i;
  }
}

void gen_ordered_permutation(perm_t *perm, size_t nr, size_t base) {
  size_t i;

  for (i = 0; i < nr; ++i) {
    perm[i] = base + i;
  }
}

int is_a_permutation(const perm_t *perm, size_t nr_elts) {
  uint8_t *vec;
  size_t vec_len = (nr_elts + 7) / 8;
  size_t i;

  vec = malloc(vec_len);
  memset(vec, 0, vec_len);

  for (i = 0; i < nr_elts; ++i) {
    size_t vec_elt = perm[i] / 8;
    size_t test_bit = 1u << (perm[i] % 8);
    if (vec[vec_elt] & test_bit) {
      free(vec);
      return 0;
    }
    vec[vec_elt] |= test_bit;
  }
  for (i = 0; i < nr_elts / 8; ++i) {
    if (vec[i] != 0xff) {
      free(vec);
      return 0;
    }
  }
  if (nr_elts % 8) {
    if (vec[vec_len - 1] != ((1u << (nr_elts % 8)) - 1)) {
      free(vec);
      return 0;
    }
  }
  free(vec);
  return 1;
}

void generate_chase_mixer(struct generate_chase_common_args *args,
                          size_t nr_mixers) {
  size_t nr_mixer_indices = args->nr_mixer_indices;
  void (*gen_permutation)(perm_t *, size_t, size_t) = args->gen_permutation;

  /* Set number of mixers rounded up to the power of two */
  args->nr_mixers = 1 << (CHAR_BIT * sizeof(long) -
                          __builtin_clzl(nr_mixers - 1));
  if (verbosity > 1)
    printf("nr_mixers = %zu\n", args->nr_mixers);
  perm_t *t = malloc(nr_mixer_indices * sizeof(*t));
  if (t == NULL) {
    fprintf(stderr, "Could not allocate %lu bytes, check stride/memory size?\n",
            nr_mixer_indices * sizeof(*t));
    exit(1);
  }
  perm_t *r = malloc(nr_mixer_indices * args->nr_mixers * sizeof(*r));
  if (r == NULL) {
    fprintf(stderr, "Could not allocate %lu bytes, check stride/memory size?\n",
            nr_mixer_indices * args->nr_mixers * sizeof(*r));
    exit(1);
  }
  size_t i;
  size_t j;

  // we arrange r in a transposed manner so that all of the
  // data for a particular mixer_idx is packed together.
  for (i = 0; i < args->nr_mixers; ++i) {
    gen_permutation(t, nr_mixer_indices, 0);
    for (j = 0; j < nr_mixer_indices; ++j) {
      r[j * args->nr_mixers + i] = t[j];
    }
  }
  free(t);

  args->mixer = r;
}

// Generate a pointer chasing sequence according to chase args. 
void *generate_chase(const struct generate_chase_common_args *args,
                     size_t mixer_idx) {
  char *arena = args->arena;
  size_t total_memory = args->total_memory;
  size_t stride = args->stride;
  size_t tlb_locality = args->tlb_locality;
  void (*gen_permutation)(perm_t *, size_t, size_t) = args->gen_permutation;
  const perm_t *mixer = args->mixer + mixer_idx * args->nr_mixers;
  size_t nr_mixer_indices = args->nr_mixer_indices;

  size_t nr_tlb_groups = total_memory / tlb_locality;
  size_t nr_elts_per_tlb = tlb_locality / stride;
  size_t nr_elts = total_memory / stride;
  perm_t *tlb_perm;
  perm_t *perm;
  size_t i;
  size_t mixer_scale = stride / nr_mixer_indices;

  if (verbosity > 1)
    printf("generating permutation of %zu elements (in %zu TLB groups)\n",
           nr_elts, nr_tlb_groups);
  tlb_perm = malloc(nr_tlb_groups * sizeof(*tlb_perm));
  gen_permutation(tlb_perm, nr_tlb_groups, 0);
  perm = malloc(nr_elts * sizeof(*perm));
  for (i = 0; i < nr_tlb_groups; ++i) {
    gen_permutation(&perm[i * nr_elts_per_tlb], nr_elts_per_tlb,
                    tlb_perm[i] * nr_elts_per_tlb);
  }
  free(tlb_perm);

  dassert(is_a_permutation(perm, nr_elts));

#define MIXED(x) ((x)*stride + mixer[(x) & (args->nr_mixers - 1)] * mixer_scale)

  if (verbosity > 1)
    printf("threading the chase (mixer_idx = %zu)\n", mixer_idx);
  for (i = 0; i < nr_elts; ++i) {
    size_t next = (i+1 == nr_elts) ? 0 : i+1;
    *(void **)(arena + MIXED(perm[i])) = (void *)(arena + MIXED(perm[next]));
  }

  free(perm);

  return arena + MIXED(0);
}

// Generates nr_mixer_indices/total_par number of permutations and switch to
// the next permutation in each iteration of the chase.
// This modification is effective in getting around CMC prefetcher.
void *generate_chase_long(const struct generate_chase_common_args *args,
                     size_t mixer_idx, size_t total_par) {
  char *arena = args->arena;
  size_t total_memory = args->total_memory;
  size_t stride = args->stride;
  size_t tlb_locality = args->tlb_locality;
  void (*gen_permutation)(perm_t *, size_t, size_t) = args->gen_permutation;
  size_t nr_mixer_indices = args->nr_mixer_indices;
  size_t nr_iteration = nr_mixer_indices / total_par;
  const perm_t *mixer = args->mixer + mixer_idx * nr_iteration * args->nr_mixers;

  size_t nr_tlb_groups = total_memory / tlb_locality;
  size_t nr_elts_per_tlb = tlb_locality / stride;
  size_t nr_elts = total_memory / stride;
  perm_t *tlb_perm;
  perm_t *perm;
  size_t i;
  size_t j;
  size_t base;
  size_t mixer_scale = stride / nr_mixer_indices;

  if (verbosity > 1)
    printf("generating permutation of %zu elements (in %zu TLB groups)\n",
           nr_elts, nr_tlb_groups);

  perm = malloc(nr_iteration * nr_elts * sizeof(*perm));
  if (perm == NULL) {
    fprintf(stderr, "Could not allocate %lu bytes\n",
            nr_iteration * nr_elts * sizeof(*perm));
    exit(1);
  }

  // Generate nr_iteration number of permutations.
  for (j = 0; j < nr_iteration; j++) {
    base = j * nr_elts;

    tlb_perm = malloc(nr_tlb_groups * sizeof(*tlb_perm));
    if (tlb_perm == NULL) {
      fprintf(stderr, "Could not allocate %lu bytes\n",
              nr_tlb_groups * sizeof(*tlb_perm));
      exit(1);
    }

    gen_permutation(tlb_perm, nr_tlb_groups, 0);

    for (i = 0; i < nr_tlb_groups; ++i) {
      gen_permutation(&perm[j * nr_elts + i * nr_elts_per_tlb], nr_elts_per_tlb,
                    base + tlb_perm[i] * nr_elts_per_tlb);
    }
    free(tlb_perm);

    dassert(is_a_permutation(perm, nr_elts));
  }

  dassert(is_a_permutation(perm, nr_iteration * nr_elts));

#define MIXED_2(x,i) ((x)*stride + mixer[i*args->nr_mixers + ((x) & (args->nr_mixers-1))] * mixer_scale)

  size_t cur = 0;
  for (i = 0; i < nr_iteration; ++i) {
    for (j = 0; j < nr_elts; ++j) {
      size_t next = cur+1 == nr_iteration * nr_elts? 0: cur+1;
      size_t i_next = j+1==nr_elts? (next==0? 0: i+1): i;
      *(void **)(arena + MIXED_2(perm[cur]%nr_elts,i)) = 
        (void *)(arena + MIXED_2(perm[next]%nr_elts,i_next));
      cur++;
    }
  }

  free(perm);

  return arena + MIXED_2(0,0);
}
