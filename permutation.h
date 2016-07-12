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
#ifndef PERMUTATION_H_INCLUDED
#define PERMUTATION_H_INCLUDED

#include <stdint.h>
#include <stdlib.h>

typedef uint32_t perm_t;

void gen_random_permutation(perm_t *perm, size_t nr, size_t base);
void gen_ordered_permutation(perm_t *perm, size_t nr, size_t base);
int is_a_permutation(const perm_t *perm, size_t nr_elts);

// regarding the mixer:  suppose we have a stride of 256 ... what we want
// to avoid is having the entire chase at offset 0 into the 256 byte element.
// otherwise we might favour one bank/branch/etc. of the memory system.
// similarly when we perform a parallel chase with multiple threads we don't
// want one of the many chases to favour a particular offset into the stride.
// so we have a "mixer" permutation.
//
// consider the arena as a large set of elements of size stride, then the naive
// "mixer" would use some fixed offset into each element for a particular
// (thread number, parallel chase) index.  the mixer is a function on (element
// index, thread number, parallel chase) which makes the offset into the element
// unpredictable.
//
// the actual mixer is implemented as a large set of permutations on the
// low bits of the element number.  the details are private to the implementation.

// these are the common args required for generating all chases (and the mixer).
struct generate_chase_common_args {
  char *arena;                  // memory used for all chases
  size_t total_memory;          // size of the arena
  size_t stride;                // size of each element
  size_t tlb_locality;          // group accesses within this range in order to
                                // amortize TLB fills
  void (*gen_permutation)(perm_t *, size_t, size_t); // function for generating
                                // permutations
                                // typically gen_random_permutation
  size_t nr_mixer_indices;      // number of mixer indices
                                // typically stride/sizeof(void*)
  const perm_t *mixer;          // the mixer function itself
};

// create the mixer table
void generate_chase_mixer(struct generate_chase_common_args *args);

// create a chase for the given mixer_idx and return its first pointer
void *generate_chase(const struct generate_chase_common_args *args, size_t mixer_idx);

//============================================================================
// Modern multicore CPUs have increasingly large caches, so the LCRNG code
// that was previously used is not sufficiently random anymore.
// Now using glibc's reentrant random number generator "random_r"
// still reproducible on the same platform, although not across systems/libs.

// RNG_BUF_SIZE sets the size of rng_buf below, which is used by initstate_r
// to decide how sophisticated a random number generator it should use: the
// larger the state array, the better the random numbers will be.
// 32 bytes was deemed to generate sufficient entropy.
#define RNG_BUF_SIZE 32
extern __thread char* rng_buf;
extern __thread struct random_data* rand_state;

static inline void rng_init(unsigned thread_num)
{
        rng_buf = (char*)calloc(1, RNG_BUF_SIZE);
        rand_state = (struct random_data*)calloc(1, sizeof(struct random_data));
        initstate_r(thread_num, rng_buf, RNG_BUF_SIZE, rand_state);
}

static inline perm_t rng_int(perm_t limit)
{
        int r;
        random_r(rand_state, &r);
        // much more uniform to use [0.,1.) multiply than use an integer modulus
        return (limit + 1) * (r * 1.0 / RAND_MAX);
}

#endif
