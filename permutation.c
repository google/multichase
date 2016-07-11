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
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "permutation.h"

// some asserts are more expensive than we want in general use, but there are a
// few i want active even in general use.
#if 1
#define dassert(x) do {} while (0)
#else
#define dassert(x) assert(x)
#endif

// XXX: declare this somewhere
extern int verbosity;

__thread char* rng_buf; // buf size (32 for now) determines the "randomness"
__thread struct random_data* rand_state; // per_thread state for random_r

//============================================================================
// a random permutation generator.  i think this algorithm is from Knuth.

void gen_random_permutation(perm_t *perm, size_t nr, size_t base)
{
	size_t i;

	for (i = 0; i < nr; ++i) {
		size_t t = rng_int(i);
		perm[i] = perm[t];
		perm[t] = base + i;
	}
}


void gen_ordered_permutation(perm_t *perm, size_t nr, size_t base)
{
        size_t i;

        for (i = 0; i < nr; ++i) {
                perm[i] = base+i;
        }
}

int is_a_permutation(const perm_t *perm, size_t nr_elts)
{
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
                if (vec[vec_len-1] != ((1u << (nr_elts % 8)) - 1)) {
                        free(vec);
                        return 0;
                }
        }
        free(vec);
        return 1;
}


// power-of-2 number of mixer permutations
#define NR_MIXERS (16384)

void generate_chase_mixer(struct generate_chase_common_args *args)
{
        size_t nr_mixer_indices = args->nr_mixer_indices;
        void (*gen_permutation)(perm_t *, size_t, size_t) = args->gen_permutation;

        perm_t *t = malloc(nr_mixer_indices * sizeof(*t));
        if (t == NULL) {
          fprintf(stderr,
                  "Could not allocate %lu bytes, check stride/memory size?\n",
                  nr_mixer_indices * sizeof(*t));
          exit(1);
        }
        perm_t *r = malloc(nr_mixer_indices * NR_MIXERS * sizeof(*r));
        if (r == NULL) {
          fprintf(stderr,
                  "Could not allocate %lu bytes, check stride/memory size?\n",
                  nr_mixer_indices * NR_MIXERS * sizeof(*r));
          exit(1);
        }
        size_t i;
        size_t j;

        // we arrange r in a transposed manner so that all of the
        // data for a particular mixer_idx is packed together.
        for (i = 0; i < NR_MIXERS; ++i) {
                gen_permutation(t, nr_mixer_indices, 0);
                for (j = 0; j < nr_mixer_indices; ++j) {
                        r[j * NR_MIXERS + i] = t[j];
                }
        }
        free(t);

        args->mixer = r;
}

void *generate_chase(const struct generate_chase_common_args *args,
        size_t mixer_idx)
{
        char *arena = args->arena;
        size_t total_memory = args->total_memory;
        size_t stride = args->stride;
        size_t tlb_locality = args->tlb_locality;
        void (*gen_permutation)(perm_t *, size_t, size_t) = args->gen_permutation;
        const perm_t *mixer = args->mixer + mixer_idx * NR_MIXERS;
        size_t nr_mixer_indices = args->nr_mixer_indices;

        size_t nr_tlb_groups = total_memory / tlb_locality;
        size_t nr_elts_per_tlb = tlb_locality / stride;
        size_t nr_elts = total_memory / stride;
        perm_t *tlb_perm;
        perm_t *perm;
        size_t i;
        perm_t *perm_inverse;
        size_t mixer_scale = stride / nr_mixer_indices;

        if (verbosity > 1) printf("generating permutation of %zu elements (in %zu TLB groups)\n", nr_elts, nr_tlb_groups);
        tlb_perm = malloc(nr_tlb_groups * sizeof(*tlb_perm));
        gen_permutation(tlb_perm, nr_tlb_groups, 0);
        perm = malloc(nr_elts * sizeof(*perm));
        for (i = 0; i < nr_tlb_groups; ++i) {
                gen_permutation(&perm[i * nr_elts_per_tlb], nr_elts_per_tlb, tlb_perm[i] * nr_elts_per_tlb);
        }
        free(tlb_perm);

        dassert(is_a_permutation(perm, nr_elts));

        if (verbosity > 1) printf("generating inverse permtuation\n");
        perm_inverse = malloc(nr_elts * sizeof(*perm));
        for (i = 0; i < nr_elts; ++i) {
                perm_inverse[perm[i]] = i;
        }

        dassert(is_a_permutation(perm_inverse, nr_elts));

#define MIXED(x) \
        ((x)*stride + mixer[(x) & (NR_MIXERS-1)]*mixer_scale)

        if (verbosity > 1) printf("threading the chase (mixer_idx = %zu)\n", mixer_idx);
        for (i = 0; i < nr_elts; ++i) {
                size_t next;
                dassert(perm[perm_inverse[i]] == i);
                assert(*(void **)(arena + MIXED(i)) == NULL);
                next = perm_inverse[i] + 1;
                next = (next == nr_elts) ? 0 : next;
                *(void **)(arena + MIXED(i)) = (void *)(arena + MIXED(perm[next]));
        }

        free(perm);
        free(perm_inverse);

        return arena + MIXED(0);
}
